package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/rand"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	maximumHTTPBody      = 64 << 10
	maximumLine          = 16 << 10
	maximumSSEEvent      = 16 << 10
	maximumAssetWire     = 512 << 10
	maximumAssetDecoded  = 1 << 20
	maximumWSUnsolicited = 16
	protocolVersion      = "2026-07-28"
	fixtureAuth          = "demo/showcase-only"
	httpConnectionLimit  = 3
)

type counters struct {
	mu                   *sync.Mutex
	HTTP                 uint64                  `json:"http_requests"`
	MCPList              uint64                  `json:"mcp_list"`
	MCPEcho              uint64                  `json:"mcp_echo"`
	MCPInvalid           uint64                  `json:"mcp_invalid"`
	Assets               uint64                  `json:"asset_requests"`
	Summary              uint64                  `json:"summary_requests"`
	WSOpened             uint64                  `json:"ws_opened"`
	WSEchoSent           uint64                  `json:"ws_echo_sent"`
	WSEchoReceived       uint64                  `json:"ws_echo_received"`
	WSUnsolicitedSummary uint64                  `json:"ws_unsolicited_summary"`
	WSUnsolicitedHello   uint64                  `json:"ws_unsolicited_hello"`
	WSPings              uint64                  `json:"ws_pings"`
	SSEOpened            uint64                  `json:"sse_opened"`
	SSEProgress          uint64                  `json:"sse_progress_events"`
	Errors               uint64                  `json:"errors"`
	Timeouts             uint64                  `json:"timeouts"`
	ErrorReasons         map[string]uint64       `json:"error_reason_counts"`
	LastError            string                  `json:"last_error,omitempty"`
	Status               map[string]uint64       `json:"http_status_counts"`
	Integrity            map[string]uint64       `json:"body_integrity_failures"`
	BodySHA256           map[string]string       `json:"body_sha256"`
	BodyBytes            map[string]uint64       `json:"body_bytes"`
	BodyWireBytes        map[string]uint64       `json:"body_wire_bytes"`
	BodyEncoding         map[string]string       `json:"body_encoding"`
	LatencyMS            latencySummary          `json:"latency_ms_recent_window"`
	WSLatencyMS          latencySummary          `json:"ws_echo_latency_ms_recent_window"`
	PerClient            map[string]clientCounts `json:"per_client"`
	BootID               string                  `json:"boot_id,omitempty"`
	SSELastEventID       string                  `json:"sse_last_event_id,omitempty"`
	latencies            []float64
	wsLatencies          []float64
	lastEventID          string
}

type latencySummary struct {
	P50   float64 `json:"p50"`
	P95   float64 `json:"p95"`
	Max   float64 `json:"max"`
	Count int     `json:"samples"`
}

type clientCounts struct {
	HTTP      uint64 `json:"http"`
	WS        uint64 `json:"ws_echo"`
	WSSummary uint64 `json:"ws_summary"`
	WSHello   uint64 `json:"ws_hello"`
	SSE       uint64 `json:"sse_events"`
	Err       uint64 `json:"errors"`
}

type runOptions struct {
	base       string
	duration   time.Duration
	clients    int
	interval   time.Duration
	local      net.IP
	output     string
	assetPath  string
	assetPaths []string
	assetMode  string
	prefetchMS int64
	stopFile   string
	startedAt  time.Time
}

type record struct {
	Type                string       `json:"type"`
	At                  time.Time    `json:"at"`
	ElapsedMS           int64        `json:"elapsed_ms,omitempty"`
	RunID               string       `json:"run_id"`
	Stage               string       `json:"stage"`
	Base                string       `json:"base"`
	Clients             int          `json:"clients"`
	HTTPConnectionLimit int          `json:"http_connection_limit"`
	AssetPaths          []string     `json:"asset_paths,omitempty"`
	AssetMode           string       `json:"asset_mode"`
	PrefetchMS          int64        `json:"asset_prefetch_ms,omitempty"`
	Interval            string       `json:"interval"`
	Counters            *counters    `json:"counters,omitempty"`
	Recovery            *probeResult `json:"recovery,omitempty"`
	Message             string       `json:"message,omitempty"`
}

type probeResult struct {
	HealthStatus int    `json:"health_status"`
	InfoStatus   int    `json:"info_status"`
	BootID       string `json:"boot_id,omitempty"`
	Error        string `json:"error,omitempty"`
}

type outputWriter struct {
	mu   sync.Mutex
	file *os.File
}

func (writer *outputWriter) write(value record) error {
	data, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if len(data) > maximumLine {
		return errors.New("JSONL record exceeds 16 KiB cap")
	}
	writer.mu.Lock()
	defer writer.mu.Unlock()
	_, err = writer.file.Write(append(data, '\n'))
	if err == nil {
		err = writer.file.Sync()
	}
	return err
}

func main() {
	base := flag.String("base", "", "Controller HTTP origin, for example http://<device-ip>")
	duration := flag.Duration("duration", 10*time.Minute, "Bounded stage duration (maximum 24h)")
	clients := flag.Int("clients", 2, "Logical clients: normal 1/2/3; mixed stress 2/4 split between WS and SSE")
	interval := flag.Duration("interval", 250*time.Millisecond, "Per-client HTTP and WS cadence")
	localAddress := flag.String("local-address", "", "Required local LAN source IPv4; direct dial, no proxy")
	outputPath := flag.String("out", "", "Append-only JSONL report path (default: run.jsonl in this module, ignored by Git)")
	assetPath := flag.String("asset", "/", "Embedded static resource to fetch and hash")
	assetMode := flag.String("asset-mode", "reload", "Static UI traffic profile: reload (re-fetch assets) or cached (prefetch assets once, then use summary reads)")
	stopFile := flag.String("stop-file", "", "Optional sentinel path; existence cancels the stage and drains clients")
	profile := flag.String("profile", "stress", "Traffic profile: rest, ui (1/2/3 clients, interval >=1s), qos, limits, ws-close, or historical mixed stress")
	headerLimit := flag.Int("http-header-limit", 8192, "Configured device HTTP header bytes for the limits profile")
	qosKey := flag.String("qos-key", "", "Configured primary admission key for the qos profile (not logged)")
	flag.Parse()
	if *profile == "qos-streams" {
		if err := runQosStreams(*base, *localAddress, *outputPath, *qosKey); err != nil {
			fatal(err)
		}
		return
	}
	if *profile == "qos" {
		if err := runQosProfile(*base, *localAddress, *outputPath, *qosKey, *duration); err != nil {
			fatal(err)
		}
		return
	}
	if *profile == "ws-close" {
		if err := runCloseProfile(*base, *localAddress, *outputPath); err != nil {
			fatal(err)
		}
		return
	}
	if *profile == "limits" {
		if err := runLimitProfile(*base, *localAddress, *outputPath, *headerLimit); err != nil {
			fatal(err)
		}
		return
	}
	if *profile != "stress" {
		if err := runNormalProfile(*profile, *base, *duration, *clients, *interval, *localAddress, *outputPath); err != nil {
			fatal(err)
		}
		return
	}

	options, err := validateOptions(*base, *duration, *clients, *interval, *localAddress, *outputPath, *assetPath, *assetMode)
	if err != nil {
		fmt.Fprintln(os.Stderr, "load harness:", err)
		os.Exit(2)
	}
	options.stopFile = *stopFile
	if options.output == "" {
		options.output = "run.jsonl"
	}
	if err := os.MkdirAll(filepath.Dir(options.output), 0755); err != nil {
		fatal(err)
	}
	resumeEventID, err := resumeCursor(options.output, options.base)
	if err != nil {
		fatal(err)
	}
	file, err := os.OpenFile(options.output, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		fatal(err)
	}
	defer file.Close()
	writer := &outputWriter{file: file}
	options.startedAt = time.Now()
	runID := fmt.Sprintf("%s-%d", options.startedAt.UTC().Format("20060102T150405Z"), os.Getpid())

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	ctx, cancel := context.WithTimeout(ctx, options.duration)
	defer cancel()

	stats := newCounters(options.clients)
	stats.setLastEventID(resumeEventID)
	client, err := newHTTPClient(options)
	if err != nil {
		fatal(err)
	}
	defer client.CloseIdleConnections()
	options.assetPaths, err = discoverAssetPaths(ctx, client, options.base, options.assetPath)
	if err != nil {
		fatal(err)
	}
	fmt.Printf("static assets selected: %s\n", strings.Join(options.assetPaths, ", "))
	if options.assetMode == "cached" {
		prefetchStarted := time.Now()
		if err = prefetchStaticAssets(ctx, client, options.base, options.assetPaths, stats); err != nil {
			fatal(fmt.Errorf("cached UI prefetch: %w", err))
		}
		options.prefetchMS = time.Since(prefetchStarted).Milliseconds()
		fmt.Printf("cached UI assets prefetched serially elapsed=%dms\n", options.prefetchMS)
	}

	identity := getBootID(ctx, client, options.base)
	stats.setBootID(identity)
	if identity == "" {
		fmt.Fprintln(os.Stderr, "boot_id unavailable; run continues with response hashes")
	}

	if err = writer.write(record{Type: "stage", At: time.Now().UTC(), RunID: runID, Stage: "mixed-load-start", Base: options.base, Clients: options.clients, HTTPConnectionLimit: httpConnectionLimit, AssetPaths: options.assetPaths, AssetMode: options.assetMode, PrefetchMS: options.prefetchMS, Interval: options.interval.String(), Message: "direct LAN; MCP list/echo/invalid read-only checks; static UI profile and summary HTTP; binary WS echo; SSE progress"}); err != nil {
		fatal(err)
	}
	fmt.Printf("load stage started run=%s clients=%d duration=%s interval=%s out=%s\n", runID, options.clients, options.duration, options.interval, options.output)

	manager := newConnectionManager()
	var workers sync.WaitGroup
	wsClients := options.clients / 2
	sseClients := options.clients / 2
	for index := 0; index < wsClients; index++ {
		workers.Add(1)
		go wsLoop(ctx, &workers, options, stats, manager, index)
	}
	for index := 0; index < sseClients; index++ {
		workers.Add(1)
		go sseLoop(ctx, &workers, options, stats, manager, wsClients+index)
	}
	for index := 0; index < options.clients; index++ {
		workers.Add(1)
		go httpLoop(ctx, &workers, options, client, stats, index)
	}

	monitorDone := make(chan struct{})
	stopCause := "duration_deadline_or_signal"
	var stopCauseMu sync.Mutex
	setStopCause := func(value string) {
		stopCauseMu.Lock()
		stopCause = value
		stopCauseMu.Unlock()
	}
	stopWatcherDone := make(chan struct{})
	go func() {
		defer close(stopWatcherDone)
		if options.stopFile == "" {
			<-ctx.Done()
			return
		}
		ticker := time.NewTicker(250 * time.Millisecond)
		defer ticker.Stop()
		for {
			if stopFileRequested(options.stopFile) {
				setStopCause("stop_file")
				cancel()
				return
			}
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
			}
		}
	}()
	go func() {
		defer close(monitorDone)
		ticker := time.NewTicker(time.Second)
		defer ticker.Stop()
		lastIdentityCheck := options.startedAt
		lastBootID := identity
		for {
			select {
			case <-ctx.Done():
				return
			case now := <-ticker.C:
				if now.Sub(lastIdentityCheck) >= 30*time.Second {
					currentBootID := getBootID(ctx, client, options.base)
					if lastBootID != "" && currentBootID != "" && currentBootID != lastBootID {
						setStopCause("boot_id_changed")
						cancel()
						return
					}
					if currentBootID != "" {
						lastBootID = currentBootID
						stats.setBootID(currentBootID)
					}
					lastIdentityCheck = now
				}
				snapshot := stats.snapshot()
				if err := writer.write(record{Type: "sample", At: now.UTC(), ElapsedMS: time.Since(options.startedAt).Milliseconds(), RunID: runID, Stage: "mixed-load", Base: options.base, Clients: options.clients, HTTPConnectionLimit: httpConnectionLimit, AssetPaths: options.assetPaths, AssetMode: options.assetMode, PrefetchMS: options.prefetchMS, Interval: options.interval.String(), Counters: &snapshot}); err != nil {
					fmt.Fprintln(os.Stderr, "JSONL write:", err)
					cancel()
					return
				}
				fmt.Printf("elapsed=%s http=%d ws=%d/%d sse=%d errors=%d timeouts=%d\n", time.Since(options.startedAt).Round(time.Second), snapshot.HTTP, snapshot.WSEchoReceived, snapshot.WSEchoSent, snapshot.SSEProgress, snapshot.Errors, snapshot.Timeouts)
			}
		}
	}()

	<-ctx.Done()
	manager.closeAll()
	client.CloseIdleConnections()
	workers.Wait()
	manager.closeAll()
	<-monitorDone
	<-stopWatcherDone

	recoveryCtx, recoveryCancel := context.WithTimeout(context.Background(), 5*time.Second)
	recovery := recoveryProbe(recoveryCtx, client, options.base)
	recoveryCancel()
	final := stats.snapshot()
	stopCauseMu.Lock()
	finalStopCause := stopCause
	stopCauseMu.Unlock()
	if err := writer.write(record{Type: "stage", At: time.Now().UTC(), ElapsedMS: time.Since(options.startedAt).Milliseconds(), RunID: runID, Stage: "mixed-load-stop-and-recovery", Base: options.base, Clients: options.clients, HTTPConnectionLimit: httpConnectionLimit, AssetPaths: options.assetPaths, AssetMode: options.assetMode, PrefetchMS: options.prefetchMS, Interval: options.interval.String(), Counters: &final, Recovery: &recovery, Message: "stop_cause=" + finalStopCause + "; all client sockets closed and goroutines drained before recovery probe"}); err != nil {
		fatal(err)
	}
	fmt.Printf("load stage stopped elapsed=%s recovery_health=%d recovery_info=%d boot_id=%s\n", time.Since(options.startedAt).Round(time.Second), recovery.HealthStatus, recovery.InfoStatus, recovery.BootID)
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}

func stopFileRequested(path string) bool {
	if path == "" {
		return false
	}
	_, err := os.Stat(path)
	if err == nil {
		return true
	}
	return !errors.Is(err, os.ErrNotExist)
}

func validateOptions(base string, duration time.Duration, clients int, interval time.Duration, localAddress, output, asset, assetMode string) (runOptions, error) {
	parsed, err := url.Parse(base)
	if err != nil || parsed.Scheme != "http" || parsed.Host == "" || parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" || parsed.Path != "" {
		return runOptions{}, errors.New("-base must be an HTTP origin without path, credentials, query, or fragment")
	}
	if parsed.Port() == "443" {
		return runOptions{}, errors.New("port 443 is forbidden for project traffic")
	}
	if duration <= 0 || duration > 24*time.Hour {
		return runOptions{}, errors.New("-duration must be greater than zero and at most 24h")
	}
	if clients != 2 && clients != 4 {
		return runOptions{}, errors.New("-clients must be 2 or 4")
	}
	if interval < 100*time.Millisecond || interval > 10*time.Second {
		return runOptions{}, errors.New("-interval must be between 100ms and 10s")
	}
	ip := net.ParseIP(localAddress)
	if ip == nil || ip.To4() == nil {
		return runOptions{}, errors.New("-local-address must be an IPv4 address")
	}
	if asset == "" || !strings.HasPrefix(asset, "/") || strings.HasPrefix(asset, "//") || strings.Contains(asset, "..") {
		return runOptions{}, errors.New("-asset must be an absolute path within the device origin")
	}
	if assetMode != "reload" && assetMode != "cached" {
		return runOptions{}, errors.New("-asset-mode must be reload or cached")
	}
	return runOptions{base: strings.TrimSuffix(base, "/"), duration: duration, clients: clients, interval: interval, local: ip, output: output, assetPath: asset, assetMode: assetMode}, nil
}

func newCounters(clients int) *counters {
	perClient := make(map[string]clientCounts, clients)
	for index := range clients {
		perClient[fmt.Sprintf("client-%d", index)] = clientCounts{}
	}
	return &counters{mu: &sync.Mutex{}, Status: make(map[string]uint64), Integrity: make(map[string]uint64), ErrorReasons: make(map[string]uint64), BodySHA256: make(map[string]string), BodyBytes: make(map[string]uint64), BodyWireBytes: make(map[string]uint64), BodyEncoding: make(map[string]string), PerClient: perClient, latencies: make([]float64, 0, 512), wsLatencies: make([]float64, 0, 512)}
}

func (stats *counters) update(fn func(*counters)) {
	stats.mu.Lock()
	fn(stats)
	stats.mu.Unlock()
}

func (stats *counters) snapshot() counters {
	stats.mu.Lock()
	defer stats.mu.Unlock()
	out := counters{HTTP: stats.HTTP, MCPList: stats.MCPList, MCPEcho: stats.MCPEcho, MCPInvalid: stats.MCPInvalid,
		Assets: stats.Assets, Summary: stats.Summary, WSOpened: stats.WSOpened, WSEchoSent: stats.WSEchoSent,
		WSEchoReceived: stats.WSEchoReceived, WSUnsolicitedSummary: stats.WSUnsolicitedSummary,
		WSUnsolicitedHello: stats.WSUnsolicitedHello, WSPings: stats.WSPings, SSEOpened: stats.SSEOpened, SSEProgress: stats.SSEProgress,
		Errors: stats.Errors, Timeouts: stats.Timeouts, ErrorReasons: cloneMap(stats.ErrorReasons), LastError: stats.LastError,
		Status: cloneMap(stats.Status), Integrity: cloneMap(stats.Integrity),
		BodySHA256: make(map[string]string, len(stats.BodySHA256)), BodyBytes: cloneMap(stats.BodyBytes),
		BodyWireBytes: cloneMap(stats.BodyWireBytes), BodyEncoding: cloneStringMap(stats.BodyEncoding),
		PerClient: make(map[string]clientCounts, len(stats.PerClient)),
		BootID:    stats.BootID, lastEventID: stats.lastEventID}
	out.SSELastEventID = stats.lastEventID
	for key, value := range stats.BodySHA256 {
		out.BodySHA256[key] = value
	}
	for key, value := range stats.PerClient {
		out.PerClient[key] = value
	}
	if len(stats.latencies) > 0 {
		values := append([]float64(nil), stats.latencies...)
		sort.Float64s(values)
		out.LatencyMS = latencySummary{P50: values[(len(values)-1)/2], P95: values[(len(values)-1)*95/100], Max: values[len(values)-1], Count: len(values)}
	}
	if len(stats.wsLatencies) > 0 {
		values := append([]float64(nil), stats.wsLatencies...)
		sort.Float64s(values)
		out.WSLatencyMS = latencySummary{P50: values[(len(values)-1)/2], P95: values[(len(values)-1)*95/100], Max: values[len(values)-1], Count: len(values)}
	}
	return out
}

func cloneMap(source map[string]uint64) map[string]uint64 {
	result := make(map[string]uint64, len(source))
	for key, value := range source {
		result[key] = value
	}
	return result
}

func cloneStringMap(source map[string]string) map[string]string {
	result := make(map[string]string, len(source))
	for key, value := range source {
		result[key] = value
	}
	return result
}

func resumeCursor(path, base string) (string, error) {
	file, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return "", err
	}
	if info.Size() == 0 {
		return "", nil
	}
	readSize := info.Size()
	if readSize > maximumLine {
		readSize = maximumLine
	}
	data := make([]byte, readSize)
	if _, err := file.ReadAt(data, info.Size()-readSize); err != nil && err != io.EOF {
		return "", err
	}
	if info.Size() > readSize {
		if newline := bytes.IndexByte(data, '\n'); newline >= 0 {
			data = data[newline+1:]
		} else {
			return "", errors.New("cannot resume: final JSONL record exceeds limit")
		}
	}
	lineEnd := bytes.LastIndexByte(data, '\n')
	if lineEnd >= 0 {
		data = data[:lineEnd]
	}
	if len(data) == 0 {
		return "", errors.New("cannot resume: no complete JSONL record at end of output")
	}
	var previous struct {
		Base     string `json:"base"`
		Counters struct {
			SSELastEventID string `json:"sse_last_event_id"`
		} `json:"counters"`
	}
	if err = json.Unmarshal(data, &previous); err != nil {
		return "", fmt.Errorf("cannot resume JSONL: %w", err)
	}
	if previous.Base != base {
		return "", fmt.Errorf("output belongs to base %q, requested %q", previous.Base, base)
	}
	return previous.Counters.SSELastEventID, nil
}

func (stats *counters) setLastEventID(eventID string) {
	stats.update(func(current *counters) { current.lastEventID = eventID })
}

func (stats *counters) currentLastEventID() string {
	stats.mu.Lock()
	defer stats.mu.Unlock()
	return stats.lastEventID
}

func newHTTPClient(options runOptions) (*http.Client, error) {
	dialer := &net.Dialer{Timeout: 2 * time.Second, KeepAlive: 20 * time.Second, LocalAddr: &net.TCPAddr{IP: options.local}}
	// 4-client mode uses two WS and two SSE sockets; three total HTTP sockets cap
	// all intended device connections at seven, including pooled keep-alives.
	transport := &http.Transport{Proxy: nil, DialContext: dialer.DialContext, MaxIdleConns: httpConnectionLimit, MaxIdleConnsPerHost: httpConnectionLimit, MaxConnsPerHost: httpConnectionLimit, IdleConnTimeout: 15 * time.Second, ResponseHeaderTimeout: 2 * time.Second, DisableCompression: true}
	return &http.Client{Transport: transport, Timeout: 4 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}, nil
}

var htmlAssetReference = regexp.MustCompile(`(?:src|href)\s*=\s*["']([^"']+)["']`)

func extractAssetPaths(document string, requested string) []string {
	paths := make([]string, 0, 4)
	seen := make(map[string]struct{})
	for _, match := range htmlAssetReference.FindAllStringSubmatch(document, 64) {
		path := match[1]
		if !strings.HasPrefix(path, "/assets/") || strings.Contains(path, "..") {
			continue
		}
		lower := strings.ToLower(path)
		if !strings.HasSuffix(lower, ".js") && !strings.HasSuffix(lower, ".css") {
			continue
		}
		if _, exists := seen[path]; !exists {
			paths = append(paths, path)
			seen[path] = struct{}{}
		}
	}
	if requested != "/" {
		if _, exists := seen[requested]; !exists {
			paths = append(paths, requested)
		}
	}
	if len(paths) == 0 {
		paths = append(paths, requested)
	}
	return paths
}

func discoverAssetPaths(ctx context.Context, client *http.Client, base, requested string) ([]string, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, base+"/", nil)
	if err != nil {
		return nil, err
	}
	response, err := client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("read asset index: %w", err)
	}
	document, _, _, readErr := readHTTPBody(response.Body, response.Header.Get("Content-Encoding"), maximumAssetWire, maximumAssetDecoded)
	response.Body.Close()
	if readErr != nil {
		return nil, readErr
	}
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("asset index HTTP %d body=%d", response.StatusCode, len(document))
	}
	return extractAssetPaths(string(document), requested), nil
}

func (stats *counters) failure(clientIndex int, err error) {
	if err == nil {
		return
	}
	stats.update(func(current *counters) {
		current.Errors++
		reason := err.Error()
		if len(reason) > 160 {
			reason = reason[:160]
		}
		current.LastError = reason
		if _, exists := current.ErrorReasons[reason]; exists || len(current.ErrorReasons) < 12 {
			current.ErrorReasons[reason]++
		} else {
			current.ErrorReasons["other_capped"]++
		}
		current.PerClient[fmt.Sprintf("client-%d", clientIndex)] = incrementErr(current.PerClient[fmt.Sprintf("client-%d", clientIndex)])
		if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
			current.Timeouts++
		}
	})
}

func incrementErr(value clientCounts) clientCounts { value.Err++; return value }

func (stats *counters) recordStatus(name string, status int) {
	stats.update(func(current *counters) { current.Status[fmt.Sprintf("%s:%d", name, status)]++ })
}

func httpLoop(ctx context.Context, workers *sync.WaitGroup, options runOptions, client *http.Client, stats *counters, index int) {
	defer workers.Done()
	ticker := time.NewTicker(options.interval)
	defer ticker.Stop()
	cycle := index
	for {
		if err := doHTTPWork(ctx, options, client, stats, index, cycle); err != nil && ctx.Err() == nil {
			stats.failure(index, err)
		}
		cycle++
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func doHTTPWork(ctx context.Context, options runOptions, client *http.Client, stats *counters, index, cycle int) error {
	switch cycle % 8 {
	case 0, 1, 2, 3:
		if options.assetMode == "cached" {
			return fetchSummary(ctx, client, options.base, index, stats)
		}
		assetPath := options.assetPaths[cycle%len(options.assetPaths)]
		return fetchBody(ctx, client, options.base+assetPath, index, stats, "asset:"+assetPath, func(status int, body []byte) error {
			if status != http.StatusOK || len(body) == 0 {
				return fmt.Errorf("asset status=%d bytes=%d", status, len(body))
			}
			return nil
		})
	case 4, 5:
		return fetchSummary(ctx, client, options.base, index, stats)
	case 6:
		return mcpRequest(ctx, client, options.base, stats, index, "tools/list")
	default:
		if err := mcpRequest(ctx, client, options.base, stats, index, "echo"); err != nil {
			return err
		}
		return mcpRequest(ctx, client, options.base, stats, index, "invalid")
	}
}

func fetchSummary(ctx context.Context, client *http.Client, base string, index int, stats *counters) error {
	return fetchBody(ctx, client, base+"/api/v2/summary", index, stats, "summary", func(status int, body []byte) error {
		if status != http.StatusOK || len(body) == 0 {
			return fmt.Errorf("summary status=%d bytes=%d", status, len(body))
		}
		return nil
	})
}

func prefetchStaticAssets(ctx context.Context, client *http.Client, base string, paths []string, stats *counters) error {
	if len(paths) == 0 {
		return errors.New("no static assets discovered")
	}
	for _, assetPath := range paths {
		if err := fetchBody(ctx, client, base+assetPath, -1, stats, "asset:"+assetPath, func(status int, body []byte) error {
			if status != http.StatusOK || len(body) == 0 {
				return fmt.Errorf("asset status=%d bytes=%d", status, len(body))
			}
			return nil
		}); err != nil {
			return fmt.Errorf("%s: %w", assetPath, err)
		}
	}
	return nil
}

func fetchBody(ctx context.Context, client *http.Client, target string, index int, stats *counters, kind string, validate func(int, []byte) error) error {
	started := time.Now()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, target, nil)
	if err != nil {
		return err
	}
	asset := strings.HasPrefix(kind, "asset:")
	if asset {
		request.Header.Set("Accept-Encoding", "gzip")
	}
	response, err := client.Do(request)
	if err != nil {
		return err
	}
	wireLimit, decodedLimit := int64(maximumHTTPBody), int64(maximumHTTPBody)
	if asset {
		wireLimit, decodedLimit = maximumAssetWire, maximumAssetDecoded
	}
	body, wireBytes, encoding, readErr := readHTTPBody(response.Body, response.Header.Get("Content-Encoding"), wireLimit, decodedLimit)
	response.Body.Close()
	if readErr != nil {
		return readErr
	}
	statusKey := fmt.Sprintf("%s:%d", kind, response.StatusCode)
	stats.update(func(current *counters) {
		current.HTTP++
		current.Status[statusKey]++
		current.latencies = boundedAppend(current.latencies, float64(time.Since(started).Microseconds())/1000, 512)
		if index >= 0 {
			id := fmt.Sprintf("client-%d", index)
			counts := current.PerClient[id]
			counts.HTTP++
			current.PerClient[id] = counts
		}
		switch {
		case strings.HasPrefix(kind, "asset:"):
			current.Assets++
		case kind == "summary":
			current.Summary++
		}
	})
	hash := sha256.Sum256(body)
	digest := hex.EncodeToString(hash[:])
	if err = validate(response.StatusCode, body); err != nil {
		stats.update(func(current *counters) {
			current.Integrity[kind]++
			current.BodySHA256[kind] = digest
			current.BodyBytes[kind] = uint64(len(body))
			current.BodyWireBytes[kind] = wireBytes
			current.BodyEncoding[kind] = encoding
		})
		return err
	}
	stats.update(func(current *counters) {
		if strings.HasPrefix(kind, "asset:") && current.BodySHA256[kind] != "" && current.BodySHA256[kind] != digest {
			current.Integrity[kind]++
		}
		current.BodySHA256[kind] = digest
		current.BodyBytes[kind] = uint64(len(body))
		current.BodyWireBytes[kind] = wireBytes
		current.BodyEncoding[kind] = encoding
	})
	return nil
}

func readHTTPBody(reader io.Reader, encoding string, maximumWireBytes, maximumDecodedBytes int64) ([]byte, uint64, string, error) {
	wire, err := io.ReadAll(io.LimitReader(reader, maximumWireBytes+1))
	if err != nil {
		return nil, uint64(len(wire)), encoding, err
	}
	if int64(len(wire)) > maximumWireBytes {
		return nil, uint64(len(wire)), encoding, fmt.Errorf("wire body exceeded %d-byte cap", maximumWireBytes)
	}
	decoded := wire
	switch strings.ToLower(strings.TrimSpace(encoding)) {
	case "", "identity":
	case "gzip":
		gzipReader, gzipErr := gzip.NewReader(bytes.NewReader(wire))
		if gzipErr != nil {
			return nil, uint64(len(wire)), encoding, fmt.Errorf("invalid gzip body: %w", gzipErr)
		}
		defer gzipReader.Close()
		decoded, err = io.ReadAll(io.LimitReader(gzipReader, maximumDecodedBytes+1))
		if err != nil {
			return nil, uint64(len(wire)), encoding, err
		}
	default:
		return nil, uint64(len(wire)), encoding, fmt.Errorf("unsupported content encoding %q", encoding)
	}
	if int64(len(decoded)) > maximumDecodedBytes {
		return nil, uint64(len(wire)), encoding, fmt.Errorf("decoded body exceeded %d-byte cap", maximumDecodedBytes)
	}
	return decoded, uint64(len(wire)), encoding, nil
}

func boundedAppend(values []float64, value float64, maximum int) []float64 {
	if len(values) == maximum {
		copy(values, values[1:])
		values = values[:maximum-1]
	}
	return append(values, value)
}

func mcpRequest(ctx context.Context, client *http.Client, base string, stats *counters, clientIndex int, kind string) error {
	var method string
	var params map[string]any
	sequence := atomic.AddUint64(&mcpSequence, 1)
	switch kind {
	case "tools/list":
		method, params = "tools/list", map[string]any{"_meta": requestMeta()}
	case "echo":
		method, params = "tools/call", map[string]any{"name": "echo", "arguments": map[string]any{"text": "load-Привет-🙂"}, "_meta": requestMeta()}
	case "invalid":
		method, params = "tools/call", map[string]any{"name": "echo", "arguments": map[string]any{}, "_meta": requestMeta()}
	default:
		return fmt.Errorf("unknown MCP work %q", kind)
	}
	body, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, base+"/mcp", bytes.NewReader(body))
	if err != nil {
		return err
	}
	request.SetBasicAuth(strings.SplitN(fixtureAuth, "/", 2)[0], strings.SplitN(fixtureAuth, "/", 2)[1])
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Accept", "application/json, text/event-stream")
	request.Header.Set("Origin", base)
	request.Header.Set("MCP-Protocol-Version", protocolVersion)
	request.Header.Set("Mcp-Method", method)
	if name, ok := params["name"].(string); ok {
		request.Header.Set("Mcp-Name", name)
	}
	started := time.Now()
	response, err := client.Do(request)
	if err != nil {
		return err
	}
	responseBody, readErr := io.ReadAll(io.LimitReader(response.Body, maximumHTTPBody+1))
	response.Body.Close()
	if readErr != nil {
		return readErr
	}
	if len(responseBody) > maximumHTTPBody {
		return fmt.Errorf("MCP %s body exceeded %d-byte cap", kind, maximumHTTPBody)
	}
	if response.StatusCode != http.StatusOK {
		stats.update(func(current *counters) {
			current.HTTP++
			current.Status[fmt.Sprintf("mcp:%d", response.StatusCode)]++
			current.BodyBytes["mcp"] = uint64(len(responseBody))
			hash := sha256.Sum256(responseBody)
			current.BodySHA256["mcp"] = hex.EncodeToString(hash[:])
		})
		return fmt.Errorf("MCP %s HTTP %d body=%d", kind, response.StatusCode, len(responseBody))
	}
	var message struct {
		Version string `json:"jsonrpc"`
		ID      uint64 `json:"id"`
		Result  struct {
			Type    string `json:"resultType"`
			IsError bool   `json:"isError"`
			Tools   []struct {
				Name string `json:"name"`
			} `json:"tools"`
			Content []struct {
				Text string `json:"text"`
			} `json:"content"`
		} `json:"result"`
		Error *struct {
			Code int `json:"code"`
		} `json:"error"`
	}
	if err := json.Unmarshal(responseBody, &message); err != nil || message.Version != "2.0" || message.ID != sequence {
		return fmt.Errorf("MCP %s invalid RPC envelope: %v", kind, err)
	}
	switch kind {
	case "tools/list":
		if message.Error != nil || message.Result.Type != "complete" || len(message.Result.Tools) < 2 {
			return errors.New("MCP tools/list did not return the read-only tools")
		}
	case "echo":
		if message.Error != nil || message.Result.Type != "complete" || message.Result.IsError || len(message.Result.Content) == 0 || message.Result.Content[0].Text != "load-Привет-🙂" {
			return errors.New("MCP echo payload mismatch")
		}
	case "invalid":
		if message.Error == nil || message.Error.Code != -32602 {
			return errors.New("invalid MCP echo arguments were not rejected with -32602")
		}
	}
	stats.update(func(current *counters) {
		current.HTTP++
		current.Status[fmt.Sprintf("mcp:%d", response.StatusCode)]++
		current.latencies = boundedAppend(current.latencies, float64(time.Since(started).Microseconds())/1000, 512)
		hash := sha256.Sum256(responseBody)
		current.BodySHA256["mcp"] = hex.EncodeToString(hash[:])
		current.BodyBytes["mcp"] = uint64(len(responseBody))
		id := fmt.Sprintf("client-%d", clientIndex)
		counts := current.PerClient[id]
		counts.HTTP++
		current.PerClient[id] = counts
		switch kind {
		case "tools/list":
			current.MCPList++
		case "echo":
			current.MCPEcho++
		case "invalid":
			current.MCPInvalid++
		}
	})
	return nil
}

var mcpSequence uint64

func requestMeta() map[string]any {
	return map[string]any{"io.modelcontextprotocol/protocolVersion": protocolVersion, "io.modelcontextprotocol/clientCapabilities": map[string]any{}}
}

func getBootID(ctx context.Context, client *http.Client, base string) string {
	request, _ := http.NewRequestWithContext(ctx, http.MethodGet, base+"/api/v2/info", nil)
	response, err := client.Do(request)
	if err != nil {
		return ""
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, maximumHTTPBody+1))
	if err != nil || response.StatusCode != http.StatusOK || len(body) > maximumHTTPBody {
		return ""
	}
	return cborBootID(body)
}

func (stats *counters) setBootID(bootID string) {
	stats.update(func(current *counters) { current.BootID = bootID })
}

func recoveryProbe(ctx context.Context, client *http.Client, base string) probeResult {
	result := probeResult{}
	for _, item := range []struct {
		path   string
		status *int
	}{{"/health", &result.HealthStatus}, {"/api/v2/info", &result.InfoStatus}} {
		request, _ := http.NewRequestWithContext(ctx, http.MethodGet, base+item.path, nil)
		response, err := client.Do(request)
		if err != nil {
			result.Error = err.Error()
			return result
		}
		*item.status = response.StatusCode
		body, readErr := io.ReadAll(io.LimitReader(response.Body, maximumHTTPBody+1))
		response.Body.Close()
		if readErr != nil || len(body) > maximumHTTPBody {
			result.Error = "recovery body read failed or exceeded cap"
			return result
		}
		if item.path == "/api/v2/info" {
			result.BootID = cborBootID(body)
		}
	}
	return result
}

type connectionManager struct {
	mu  sync.Mutex
	all []net.Conn
}

func newConnectionManager() *connectionManager { return &connectionManager{} }

func (manager *connectionManager) add(connection net.Conn) {
	manager.mu.Lock()
	manager.all = append(manager.all, connection)
	manager.mu.Unlock()
}

func (manager *connectionManager) remove(connection net.Conn) {
	manager.mu.Lock()
	for index, item := range manager.all {
		if item == connection {
			manager.all = append(manager.all[:index], manager.all[index+1:]...)
			break
		}
	}
	manager.mu.Unlock()
}

func (manager *connectionManager) closeAll() {
	manager.mu.Lock()
	all := append([]net.Conn(nil), manager.all...)
	manager.all = nil
	manager.mu.Unlock()
	for _, connection := range all {
		_ = connection.Close()
	}
}

func connect(options runOptions, path, lastEventID string) (net.Conn, *bufio.Reader, *http.Response, error) {
	return connectWithKey(options, path, lastEventID, "")
}

func connectWithKey(options runOptions, path, lastEventID, admissionKey string) (net.Conn, *bufio.Reader, *http.Response, error) {
	parsed, err := url.Parse(options.base)
	if err != nil {
		return nil, nil, nil, err
	}
	host := parsed.Host
	if !strings.Contains(host, ":") {
		host += ":80"
	}
	dialer := net.Dialer{Timeout: 2 * time.Second, KeepAlive: 20 * time.Second, LocalAddr: &net.TCPAddr{IP: options.local}}
	connection, err := dialer.Dial("tcp", host)
	if err != nil {
		return nil, nil, nil, err
	}
	_ = connection.SetDeadline(time.Now().Add(4 * time.Second))
	request, _ := http.NewRequest(http.MethodGet, options.base+path, nil)
	if admissionKey != "" {
		request.Header.Set("X-Client-Key", admissionKey)
	}
	if path == "/api/v2/events" {
		request.Header.Set("Accept", "text/event-stream")
		if lastEventID != "" {
			request.Header.Set("Last-Event-ID", lastEventID)
		}
	}
	if path == "/ws" {
		randomKey := make([]byte, 16)
		if _, err := rand.Read(randomKey); err != nil {
			connection.Close()
			return nil, nil, nil, fmt.Errorf("generate WS key: %w", err)
		}
		key := base64.StdEncoding.EncodeToString(randomKey)
		request.Header.Set("Upgrade", "websocket")
		request.Header.Set("Connection", "Upgrade")
		request.Header.Set("Sec-WebSocket-Version", "13")
		request.Header.Set("Sec-WebSocket-Key", key)
	}
	if err = request.Write(connection); err != nil {
		connection.Close()
		return nil, nil, nil, err
	}
	reader := bufio.NewReaderSize(connection, 4096)
	response, err := http.ReadResponse(reader, request)
	if err != nil {
		connection.Close()
		return nil, nil, nil, err
	}
	if path == "/ws" && response.StatusCode == http.StatusSwitchingProtocols {
		if err := validateWebSocketHandshake(request.Header.Get("Sec-WebSocket-Key"), response.Header.Get("Sec-WebSocket-Accept")); err != nil {
			connection.Close()
			return nil, nil, nil, err
		}
	}
	if path == "/api/v2/events" {
		if response.StatusCode == http.StatusOK {
			reader = bufio.NewReaderSize(response.Body, 4096)
		} else if admissionKey == "" {
			_ = response.Body.Close()
		}
	}
	return connection, reader, response, nil
}

func expectedWebSocketAccept(key string) (string, error) {
	rawKey, err := base64.StdEncoding.DecodeString(key)
	if err != nil || len(rawKey) != 16 {
		return "", errors.New("Sec-WebSocket-Key must be base64 for exactly 16 bytes")
	}
	digest := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	return base64.StdEncoding.EncodeToString(digest[:]), nil
}

func validateWebSocketHandshake(key, accept string) error {
	expected, err := expectedWebSocketAccept(key)
	if err != nil {
		return err
	}
	if accept != expected {
		return errors.New("Sec-WebSocket-Accept mismatch")
	}
	return nil
}

func wsLoop(ctx context.Context, workers *sync.WaitGroup, options runOptions, stats *counters, manager *connectionManager, clientIndex int) {
	defer workers.Done()
	ticker := time.NewTicker(options.interval)
	defer ticker.Stop()
	var connection net.Conn
	var reader *bufio.Reader
	sequence := uint64(0)
	defer func() {
		if connection != nil {
			manager.remove(connection)
			_ = connection.Close()
		}
	}()
	for {
		if ctx.Err() != nil {
			return
		}
		if connection == nil {
			var response *http.Response
			var err error
			connection, reader, response, err = connect(options, "/ws", "")
			if response != nil {
				stats.recordStatus("ws_handshake", response.StatusCode)
			}
			if err == nil && response.StatusCode != http.StatusSwitchingProtocols {
				err = fmt.Errorf("WS handshake HTTP %d", response.StatusCode)
			}
			if err != nil {
				if ctx.Err() != nil {
					if connection != nil {
						_ = connection.Close()
						connection = nil
					}
					return
				}
				stats.failure(clientIndex, err)
				if connection != nil {
					_ = connection.Close()
					connection = nil
				}
				select {
				case <-ctx.Done():
					return
				case <-time.After(time.Second):
					continue
				}
			}
			if ctx.Err() != nil {
				_ = connection.Close()
				connection = nil
				return
			}
			manager.add(connection)
			stats.update(func(current *counters) { current.WSOpened++ })
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			sequence++
			requestID := fmt.Sprintf("%d-%d", clientIndex, sequence)
			payloadText := fmt.Sprintf("ws-%s-%d", requestID, time.Now().UnixNano())
			started := time.Now()
			outcomes, err := wsExchange(connection, reader, echoCBOR(requestID, payloadText))
			if err != nil {
				if !shouldReportNetworkFailure(ctx, err) {
					return
				}
				stats.failure(clientIndex, err)
				manager.remove(connection)
				_ = connection.Close()
				connection = nil
				continue
			}
			stats.update(func(current *counters) {
				current.WSEchoSent++
				current.WSEchoReceived++
				current.WSUnsolicitedSummary += uint64(outcomes.summaries)
				current.WSUnsolicitedHello += uint64(outcomes.hellos)
				current.WSPings += uint64(outcomes.pings)
				current.wsLatencies = boundedAppend(current.wsLatencies, float64(time.Since(started).Microseconds())/1000, 512)
				id := fmt.Sprintf("client-%d", clientIndex)
				counts := current.PerClient[id]
				counts.WS++
				counts.WSSummary += uint64(outcomes.summaries)
				counts.WSHello += uint64(outcomes.hellos)
				current.PerClient[id] = counts
			})
		}
	}
}

func shouldReportNetworkFailure(ctx context.Context, err error) bool {
	return err != nil && ctx.Err() == nil
}

type wsOutcomes struct{ summaries, hellos, pings int }

type wsFrame struct {
	opcode  byte
	payload []byte
}

func writeFull(connection net.Conn, payload []byte) error {
	for len(payload) > 0 {
		written, err := connection.Write(payload)
		if err != nil {
			return err
		}
		if written == 0 {
			return io.ErrShortWrite
		}
		payload = payload[written:]
	}
	return nil
}

func writeClientWSFrame(connection net.Conn, opcode byte, payload []byte) error {
	control := opcode >= 8
	if control && len(payload) > 125 {
		return errors.New("WebSocket control payload exceeded 125 bytes")
	}
	mask := make([]byte, 4)
	if _, err := rand.Read(mask); err != nil {
		return err
	}
	frame := []byte{0x80 | opcode}
	switch {
	case len(payload) < 126:
		frame = append(frame, 0x80|byte(len(payload)))
	case len(payload) <= 65535:
		frame = append(frame, 0x80|126, byte(len(payload)>>8), byte(len(payload)))
	default:
		frame = append(frame, 0x80|127, 0, 0, 0, 0, byte(uint64(len(payload))>>24), byte(uint64(len(payload))>>16), byte(uint64(len(payload))>>8), byte(len(payload)))
	}
	frame = append(frame, mask...)
	for index, value := range payload {
		frame = append(frame, value^mask[index%4])
	}
	return writeFull(connection, frame)
}

func readWSFrame(reader *bufio.Reader) (wsFrame, error) {
	header := make([]byte, 2)
	if _, err := io.ReadFull(reader, header); err != nil {
		return wsFrame{}, err
	}
	if header[0]&0x70 != 0 || header[0]&0x80 == 0 || header[1]&0x80 != 0 {
		return wsFrame{}, fmt.Errorf("invalid server WebSocket frame header %x", header)
	}
	opcode := header[0] & 0x0f
	length := uint64(header[1] & 0x7f)
	if length == 126 {
		extended := make([]byte, 2)
		if _, err := io.ReadFull(reader, extended); err != nil {
			return wsFrame{}, err
		}
		length = uint64(extended[0])<<8 | uint64(extended[1])
	} else if length == 127 {
		extended := make([]byte, 8)
		if _, err := io.ReadFull(reader, extended); err != nil {
			return wsFrame{}, err
		}
		if extended[0]&0x80 != 0 {
			return wsFrame{}, errors.New("invalid 64-bit WebSocket payload length")
		}
		for _, value := range extended {
			length = length<<8 | uint64(value)
		}
	}
	if length > maximumHTTPBody {
		return wsFrame{}, errors.New("WebSocket frame exceeded 64 KiB cap")
	}
	if opcode >= 8 && (!((header[0] & 0x80) != 0) || length > 125) {
		return wsFrame{}, errors.New("invalid fragmented or oversized WebSocket control frame")
	}
	payload := make([]byte, int(length))
	if _, err := io.ReadFull(reader, payload); err != nil {
		return wsFrame{}, err
	}
	return wsFrame{opcode: opcode, payload: payload}, nil
}

func wsUnsolicitedKind(body []byte) (string, error) {
	value, err := decodeCBOR(body)
	if err != nil {
		return "", err
	}
	root, ok := value.(map[any]any)
	if !ok || root["v"] != uint64(2) {
		return "", errors.New("invalid unsolicited CBOR envelope")
	}
	kind, _ := root["type"].(string)
	switch kind {
	case "summary":
		bootID, bootOK := root["boot_id"].(string)
		_, sequenceOK := root["seq"].(uint64)
		_, capturedOK := root["captured_ms"].(uint64)
		if root["source"] != "device" || !bootOK || bootID == "" || !sequenceOK || !capturedOK {
			return "", errors.New("invalid device summary envelope")
		}
		if _, ok := root["data"].(map[any]any); !ok {
			return "", errors.New("device summary data must be a CBOR map")
		}
		return kind, nil
	case "hello":
		if source, exists := root["source"]; exists && source != "device" {
			return "", errors.New("invalid hello source")
		}
		return kind, nil
	default:
		return "", fmt.Errorf("unexpected unsolicited WebSocket message type %q", kind)
	}
}

func wsExchange(connection net.Conn, reader *bufio.Reader, payload []byte) (wsOutcomes, error) {
	outcomes := wsOutcomes{}
	_ = connection.SetDeadline(time.Now().Add(3 * time.Second))
	if err := writeClientWSFrame(connection, 2, payload); err != nil {
		return outcomes, err
	}
	requestValue, err := decodeCBOR(payload)
	if err != nil {
		return outcomes, err
	}
	requestMap := requestValue.(map[any]any)
	requestData := requestMap["data"].(map[any]any)
	for frameCount := 0; frameCount <= maximumWSUnsolicited; frameCount++ {
		frame, readErr := readWSFrame(reader)
		if readErr != nil {
			return outcomes, readErr
		}
		switch frame.opcode {
		case 9:
			if err := writeClientWSFrame(connection, 10, frame.payload); err != nil {
				return outcomes, err
			}
			outcomes.pings++
			continue
		case 10:
			continue
		case 8:
			_ = writeClientWSFrame(connection, 8, frame.payload)
			return outcomes, errors.New("server closed WebSocket during echo exchange")
		case 1:
			if string(frame.payload) != "hello" {
				return outcomes, errors.New("unexpected WebSocket text message")
			}
			outcomes.hellos++
			continue
		case 2:
			value, decodeErr := decodeCBOR(frame.payload)
			if decodeErr != nil {
				return outcomes, decodeErr
			}
			message, ok := value.(map[any]any)
			if !ok {
				return outcomes, errors.New("WebSocket CBOR message is not a map")
			}
			if message["type"] == "echo.response" {
				if message["request_id"] != requestMap["request_id"] {
					return outcomes, errors.New("WebSocket echo request_id mismatch")
				}
				data, ok := message["data"].(map[any]any)
				if !ok || data["text"] != requestData["text"] {
					return outcomes, errors.New("WS exact echo body mismatch")
				}
				return outcomes, nil
			}
			kind, kindErr := wsUnsolicitedKind(frame.payload)
			if kindErr != nil {
				return outcomes, kindErr
			}
			if kind == "summary" {
				outcomes.summaries++
			} else {
				outcomes.hellos++
			}
			continue
		default:
			return outcomes, fmt.Errorf("unexpected WebSocket opcode %d", frame.opcode)
		}
	}
	return outcomes, fmt.Errorf("more than %d unsolicited WebSocket frames before echo", maximumWSUnsolicited)
}

func sseLoop(ctx context.Context, workers *sync.WaitGroup, options runOptions, stats *counters, manager *connectionManager, clientIndex int) {
	defer workers.Done()
	lastID := stats.currentLastEventID()
	for ctx.Err() == nil {
		connection, reader, response, err := connect(options, "/api/v2/events", lastID)
		if response != nil {
			stats.recordStatus("sse_open", response.StatusCode)
		}
		if err == nil && response.StatusCode != http.StatusOK {
			err = fmt.Errorf("SSE HTTP %d", response.StatusCode)
		}
		if err == nil && !strings.HasPrefix(strings.ToLower(response.Header.Get("Content-Type")), "text/event-stream") {
			err = errors.New("SSE content type mismatch")
		}
		if err != nil {
			if ctx.Err() != nil {
				if connection != nil {
					_ = connection.Close()
				}
				return
			}
			stats.failure(clientIndex, err)
			if connection != nil {
				_ = connection.Close()
			}
			select {
			case <-ctx.Done():
				return
			case <-time.After(time.Second):
				continue
			}
		}
		if ctx.Err() != nil {
			_ = connection.Close()
			return
		}
		manager.add(connection)
		stats.update(func(current *counters) { current.SSEOpened++ })
		for ctx.Err() == nil {
			_ = connection.SetReadDeadline(time.Now().Add(4 * time.Second))
			eventID, data, err := readSSEEvent(reader)
			if err != nil {
				if shouldReportNetworkFailure(ctx, err) {
					stats.failure(clientIndex, err)
				}
				break
			}
			if eventID != "" {
				lastID = eventID
			}
			if len(data) == 0 {
				continue
			}
			stats.update(func(current *counters) {
				current.SSEProgress++
				current.lastEventID = lastID
				hash := sha256.Sum256([]byte(data))
				current.BodySHA256["sse_data"] = hex.EncodeToString(hash[:])
				current.BodyBytes["sse_data"] = uint64(len(data))
				id := fmt.Sprintf("client-%d", clientIndex)
				counts := current.PerClient[id]
				counts.SSE++
				current.PerClient[id] = counts
			})
		}
		manager.remove(connection)
		_ = connection.Close()
		if ctx.Err() == nil {
			select {
			case <-ctx.Done():
				return
			case <-time.After(time.Second):
			}
		}
	}
}

func readSSEEvent(reader *bufio.Reader) (string, string, error) {
	var eventID string
	var data []string
	eventBytes := 0
	for {
		line, err := readBoundedLine(reader, 4096)
		if err != nil {
			return "", "", err
		}
		line = strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r")
		if line == "" {
			if len(data) == 0 {
				continue
			}
			return eventID, strings.Join(data, "\n"), nil
		}
		if strings.HasPrefix(line, ":") {
			continue
		}
		eventBytes += len(line)
		if eventBytes > maximumSSEEvent {
			return "", "", fmt.Errorf("SSE event exceeded %d-byte cap", maximumSSEEvent)
		}
		key, value, found := strings.Cut(line, ":")
		if !found {
			value = ""
		}
		value = strings.TrimPrefix(value, " ")
		switch key {
		case "id":
			eventID = value
		case "data":
			data = append(data, value)
		}
	}
}

func readBoundedLine(reader *bufio.Reader, maximum int) (string, error) {
	var line []byte
	for len(line) <= maximum {
		part, more, err := reader.ReadLine()
		if err != nil {
			return "", err
		}
		line = append(line, part...)
		if !more {
			return string(append(line, '\n')), nil
		}
	}
	return "", fmt.Errorf("protocol line exceeded %d-byte cap", maximum)
}

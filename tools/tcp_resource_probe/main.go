// Read-only Linux process observer. It never signals the observed process.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const (
	maximumOutput = 16 << 20
	maximumLine   = 4096
	windowSamples = 5
)

type processStat struct {
	startTicks uint64
	cpuTicks   uint64
}

type snapshot struct {
	ElapsedMS    int64    `json:"elapsed_ms"`
	RSSKiB       uint64   `json:"rss_kib"`
	HighWaterKiB uint64   `json:"high_water_kib"`
	Threads      uint64   `json:"threads"`
	Descriptors  uint64   `json:"descriptors"`
	FDLowerBound bool     `json:"fd_lower_bound,omitempty"`
	PrivateKiB   *uint64  `json:"private_kib,omitempty"`
	PrivateError string   `json:"private_error,omitempty"`
	CPUTicks     uint64   `json:"cpu_ticks"`
	CPUPercent   *float64 `json:"cpu_percent_one_core,omitempty"`
	health       *healthSample
}

type healthSample struct {
	LatencyMS float64 `json:"latency_ms"`
	Status    int     `json:"status"`
	BodyBytes int     `json:"body_bytes"`
	SHA256    string  `json:"sha256,omitempty"`
	TooLarge  bool    `json:"body_too_large,omitempty"`
	Error     string  `json:"error,omitempty"`
}

type metricRange struct {
	Count        uint64  `json:"samples"`
	Minimum      uint64  `json:"min"`
	Maximum      uint64  `json:"max"`
	First        uint64  `json:"first"`
	Last         uint64  `json:"last"`
	InitialMean  float64 `json:"initial_window_mean"`
	TailMean     float64 `json:"tail_window_mean"`
	Trend        float64 `json:"trend_per_second"`
	initialSum   float64
	tail         [windowSamples]uint64
	x, xx, y, xy float64
}

func (metric *metricRange) add(value uint64, seconds float64) {
	if metric.Count == 0 {
		metric.Minimum, metric.Maximum, metric.First = value, value, value
	}
	if value < metric.Minimum {
		metric.Minimum = value
	}
	if value > metric.Maximum {
		metric.Maximum = value
	}
	if metric.Count < windowSamples {
		metric.initialSum += float64(value)
	}
	metric.tail[metric.Count%windowSamples] = value
	metric.Last = value
	metric.Count++
	metric.x += seconds
	metric.xx += seconds * seconds
	metric.y += float64(value)
	metric.xy += seconds * float64(value)
}

func (metric *metricRange) finish() {
	if metric.Count == 0 {
		return
	}
	window := metric.Count
	if window > windowSamples {
		window = windowSamples
	}
	metric.InitialMean = metric.initialSum / float64(window)
	for index := uint64(0); index < window; index++ {
		metric.TailMean += float64(metric.tail[index]) / float64(window)
	}
	denominator := float64(metric.Count)*metric.xx - metric.x*metric.x
	if denominator > 0 && metric.Minimum != metric.Maximum {
		metric.Trend = (float64(metric.Count)*metric.xy - metric.x*metric.y) / denominator
	}
}

type output struct{ written int }

var errOutputLimit = errors.New("JSONL output limit")
var errPIDReused = errors.New("PID reused")

func (writer *output) emit(value any, final bool) error {
	data, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if len(data) > maximumLine {
		return errors.New("JSONL line limit")
	}
	limit := maximumOutput - maximumLine
	if final {
		limit = maximumOutput
	}
	if writer.written+len(data)+1 > limit {
		return errOutputLimit
	}
	data = append(data, '\n')
	count, err := os.Stdout.Write(data)
	writer.written += count
	return err
}

func shortError(err error) string {
	if err == nil {
		return ""
	}
	text := err.Error()
	if len(text) > 160 {
		text = text[:160]
	}
	return text
}

func boundedFile(path string, maximum int64) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, maximum+1))
	if err == nil && int64(len(data)) > maximum {
		err = errors.New("proc file size limit")
	}
	return data, err
}

func readStat(directory string) (processStat, error) {
	data, err := boundedFile(directory+"/stat", 4096)
	if err != nil {
		return processStat{}, err
	}
	end := strings.LastIndexByte(string(data), ')')
	if end < 0 {
		return processStat{}, errors.New("malformed proc stat comm")
	}
	fields := strings.Fields(string(data[end+1:]))
	if len(fields) < 20 {
		return processStat{}, errors.New("short proc stat")
	}
	user, firstErr := strconv.ParseUint(fields[11], 10, 64)
	system, secondErr := strconv.ParseUint(fields[12], 10, 64)
	start, thirdErr := strconv.ParseUint(fields[19], 10, 64)
	if firstErr != nil || secondErr != nil || thirdErr != nil || user > math.MaxUint64-system {
		return processStat{}, errors.New("invalid proc stat counters")
	}
	return processStat{start, user + system}, nil
}

func fields(data []byte) map[string]uint64 {
	result := make(map[string]uint64)
	for _, line := range strings.Split(string(data), "\n") {
		parts := strings.Fields(line)
		if len(parts) < 2 {
			continue
		}
		if value, err := strconv.ParseUint(parts[1], 10, 64); err == nil {
			result[strings.TrimSuffix(parts[0], ":")] = value
		}
	}
	return result
}

func descriptors(directory string) (uint64, bool, error) {
	file, err := os.Open(directory + "/fd")
	if err != nil {
		return 0, false, err
	}
	defer file.Close()
	var count uint64
	for count < 65536 {
		entries, err := file.ReadDir(128)
		count += uint64(len(entries))
		if err == io.EOF {
			return count, false, nil
		}
		if err != nil {
			return count, false, err
		}
	}
	return count, true, nil
}

func readSnapshot(directory string, identity uint64, private bool) (snapshot, error) {
	before, err := readStat(directory)
	if err != nil {
		return snapshot{}, err
	}
	if before.startTicks != identity {
		return snapshot{}, fmt.Errorf("%w: starttime changed", errPIDReused)
	}
	data, err := boundedFile(directory+"/status", 32768)
	if err != nil {
		return snapshot{}, err
	}
	values := fields(data)
	rss, haveRSS := values["VmRSS"]
	water, haveWater := values["VmHWM"]
	threads, haveThreads := values["Threads"]
	if !haveRSS || !haveWater || !haveThreads {
		return snapshot{}, errors.New("process has no live RSS/HWM/thread status")
	}
	result := snapshot{RSSKiB: rss, HighWaterKiB: water, Threads: threads}
	result.Descriptors, result.FDLowerBound, err = descriptors(directory)
	if err != nil {
		return snapshot{}, err
	}
	if private {
		data, err := boundedFile(directory+"/smaps_rollup", 32768)
		if err == nil {
			values := fields(data)
			_, haveClean := values["Private_Clean"]
			_, haveDirty := values["Private_Dirty"]
			if haveClean && haveDirty {
				value := values["Private_Clean"] + values["Private_Dirty"] + values["Private_Hugetlb"]
				result.PrivateKiB = &value
			} else {
				err = errors.New("private memory fields unavailable")
			}
		}
		result.PrivateError = shortError(err)
	}
	after, err := readStat(directory)
	if err != nil {
		return snapshot{}, err
	}
	if after.startTicks != identity {
		return snapshot{}, fmt.Errorf("%w: starttime changed during snapshot", errPIDReused)
	}
	result.CPUTicks = after.cpuTicks
	return result, nil
}

type clockOutput struct {
	bytes [64]byte
	used  int
	full  bool
}

func (writer *clockOutput) Write(data []byte) (int, error) {
	amount := copy(writer.bytes[writer.used:], data)
	writer.used += amount
	writer.full = writer.full || amount != len(data)
	return len(data), nil
}

func clockTicks() (uint64, error) {
	command := exec.Command("/usr/bin/getconf", "CLK_TCK")
	var output clockOutput
	command.Stdout, command.Stderr = &output, io.Discard
	if err := command.Run(); err != nil {
		return 0, err
	}
	value, err := strconv.ParseUint(strings.TrimSpace(string(output.bytes[:output.used])), 10, 64)
	if err != nil || output.full || value == 0 {
		return 0, errors.New("invalid getconf CLK_TCK")
	}
	return value, nil
}

func healthClient(address string) (*http.Client, error) {
	parsed, err := url.Parse(address)
	if err != nil || len(address) > 128 || parsed.Scheme != "http" || parsed.Hostname() != "127.0.0.1" ||
		parsed.Path != "/health" || parsed.RawPath != "" || parsed.RawQuery != "" || parsed.Fragment != "" ||
		parsed.ForceQuery || parsed.User != nil || parsed.Opaque != "" {
		return nil, errors.New("health must be plain http://127.0.0.1[:port]/health without credentials/query/fragment")
	}
	port := parsed.Port()
	if port == "" {
		port = "80"
	}
	number, err := strconv.Atoi(port)
	if err != nil || number < 1 || number > 65535 {
		return nil, errors.New("invalid health port")
	}
	expected := net.JoinHostPort("127.0.0.1", port)
	dialer := &net.Dialer{Timeout: time.Second}
	transport := &http.Transport{Proxy: nil, DisableCompression: true, MaxResponseHeaderBytes: 8192,
		MaxIdleConns: 1, MaxIdleConnsPerHost: 1, MaxConnsPerHost: 1, ResponseHeaderTimeout: time.Second}
	transport.DialContext = func(ctx context.Context, network, address string) (net.Conn, error) {
		if address != expected {
			return nil, errors.New("non-loopback dial refused")
		}
		return dialer.DialContext(ctx, "tcp4", address)
	}
	return &http.Client{Transport: transport, Timeout: time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}, nil
}

func health(ctx context.Context, client *http.Client, address string) healthSample {
	started := time.Now()
	result := healthSample{}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, address, nil)
	if err == nil {
		var response *http.Response
		response, err = client.Do(request)
		if err == nil {
			result.Status = response.StatusCode
			var body []byte
			body, err = io.ReadAll(io.LimitReader(response.Body, 4097))
			response.Body.Close()
			result.BodyBytes, result.TooLarge = len(body), len(body) > 4096
			if !result.TooLarge {
				hash := sha256.Sum256(body)
				result.SHA256 = hex.EncodeToString(hash[:])
			}
		}
	}
	result.Error, result.LatencyMS = shortError(err), float64(time.Since(started).Microseconds())/1000
	return result
}

func run() error {
	pid := flag.Int("pid", 0, "observed Linux PID (required)")
	duration := flag.Duration("duration", 20*time.Second, "observation duration,100ms..1800s")
	interval := flag.Duration("interval", 500*time.Millisecond, "sampling interval,100ms..duration")
	healthAddress := flag.String("health", "", "optional numeric loopback /health URL, at most one request per second")
	private := flag.Bool("private-memory", false, "also read smaps_rollup private clean/dirty/hugetlb KiB when permitted")
	flag.Parse()
	if runtime.GOOS != "linux" || flag.NArg() != 0 || *pid <= 0 || *pid > 1<<30 ||
		*duration < 100*time.Millisecond || *duration > 1800*time.Second ||
		*interval < 100*time.Millisecond || *interval > *duration {
		return errors.New("requires Linux, positive PID<=2^30, duration100ms..1800s and interval100ms..duration")
	}
	var client *http.Client
	if *healthAddress != "" {
		var err error
		client, err = healthClient(*healthAddress)
		if err != nil {
			return err
		}
		defer client.CloseIdleConnections()
	}
	ticks, err := clockTicks()
	if err != nil {
		return err
	}
	directory := "/proc/" + strconv.Itoa(*pid)
	identity, err := readStat(directory)
	if err != nil {
		return err
	}
	writer := &output{}
	if err := writer.emit(map[string]any{"event": "config", "pid": *pid, "start_ticks": identity.startTicks,
		"clock_ticks_per_second": ticks, "duration_ms": duration.Milliseconds(), "interval_ms": interval.Milliseconds(),
		"observer_started_utc": time.Now().UTC().Format(time.RFC3339Nano),
		"health":               *healthAddress, "private_memory": *private, "maximum_output_bytes": maximumOutput,
		"window_samples": windowSamples}, false); err != nil {
		return err
	}
	started := time.Now()
	deadline := started.Add(*duration)
	next, nextHealth := started, started
	var previous snapshot
	var previousTime, firstTime time.Time
	var firstTicks, count, healthCount, healthErrors, skipped, cappedDescriptors uint64
	var rss, water, threads, fds, privateRange metricRange
	reason, errorText := "duration", ""
	for count < 18001 {
		if !time.Now().Before(deadline) {
			break
		}
		current, err := readSnapshot(directory, identity.startTicks, *private)
		if err != nil {
			reason, errorText = "proc_read_error", shortError(err)
			if errors.Is(err, errPIDReused) {
				reason = "pid_reused"
			} else if os.IsNotExist(err) {
				reason = "process_exit"
			}
			break
		}
		now := time.Now()
		current.ElapsedMS = now.Sub(started).Milliseconds()
		if count != 0 {
			if current.CPUTicks < previous.CPUTicks {
				reason, errorText = "cpu_ticks_regressed", "CPU counter decreased without a new starttime"
				break
			}
			percent := 100 * float64(current.CPUTicks-previous.CPUTicks) / float64(ticks) / now.Sub(previousTime).Seconds()
			current.CPUPercent = &percent
		} else {
			firstTicks, firstTime = current.CPUTicks, now
		}
		if client != nil && !now.Before(nextHealth) {
			ctx, cancel := context.WithDeadline(context.Background(), deadline)
			result := health(ctx, client, *healthAddress)
			cancel()
			current.health = &result
			healthCount++
			if result.Error != "" || result.TooLarge || result.Status != http.StatusOK {
				healthErrors++
			}
			nextHealth = now.Add(time.Second)
		}
		if err := writer.emit(struct {
			Event string `json:"event"`
			snapshot
			Health *healthSample `json:"health,omitempty"`
		}{"sample", current, current.health}, false); err != nil {
			reason, errorText = "output_error", shortError(err)
			break
		}
		seconds := now.Sub(started).Seconds()
		rss.add(current.RSSKiB, seconds)
		water.add(current.HighWaterKiB, seconds)
		threads.add(current.Threads, seconds)
		fds.add(current.Descriptors, seconds)
		if current.FDLowerBound {
			cappedDescriptors++
		}
		if current.PrivateKiB != nil {
			privateRange.add(*current.PrivateKiB, seconds)
		}
		previous, previousTime = current, now
		count++
		next = next.Add(*interval)
		if now := time.Now(); !next.After(now) {
			missed := uint64(now.Sub(next) / *interval) + 1
			skipped += missed
			next = next.Add(time.Duration(missed) * *interval)
		}
		wake := next
		if deadline.Before(wake) {
			wake = deadline
		}
		if delay := time.Until(wake); delay > 0 {
			time.Sleep(delay)
		}
	}
	for _, metric := range []*metricRange{&rss, &water, &threads, &fds, &privateRange} {
		metric.finish()
	}
	cpuSeconds, averageCPU := 0.0, 0.0
	if count > 1 {
		cpuSeconds = float64(previous.CPUTicks-firstTicks) / float64(ticks)
		averageCPU = 100 * cpuSeconds / previousTime.Sub(firstTime).Seconds()
	}
	if err := writer.emit(map[string]any{"event": "summary", "pid": *pid, "start_ticks": identity.startTicks,
		"reason": reason, "error": errorText, "samples": count, "elapsed_ms": time.Since(started).Milliseconds(),
		"skipped_intervals": skipped, "rss_kib": rss, "high_water_kib": water, "threads": threads,
		"fd_capped_samples": cappedDescriptors,
		"descriptors":       fds, "private_kib": privateRange, "cpu_seconds_delta": cpuSeconds,
		"cpu_percent_one_core_average": averageCPU, "health_samples": healthCount, "health_errors": healthErrors,
		"window_meaning": "initial/tail windows are observations, not proof that the application drained"}, true); err != nil {
		return err
	}
	if reason != "duration" {
		return errors.New(reason + ": " + errorText)
	}
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, shortError(err))
		os.Exit(1)
	}
}

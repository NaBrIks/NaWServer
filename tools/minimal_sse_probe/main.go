// Bounded acceptance probe for examples/minimal, not integration_device.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"syscall"
	"time"
)

const fixtureAuth = "Basic dGVzdDp0ZXN0LW9ubHk="
const operationTimeout = 4 * time.Second
const drainTimeout = 7 * time.Second

type probe struct {
	ctx       context.Context
	base      string
	host      string
	client    *http.Client
	lastState stats
	haveState bool
}

type stream struct {
	conn     net.Conn
	response *http.Response
	reader   *bufio.Reader
	id       string
	limit    time.Time
}

type result struct {
	Status           string `json:"status"`
	Profile          string `json:"profile"`
	Stage            string `json:"stage"`
	RequestedClients int    `json:"requested_clients"`
	URL              string `json:"url"`
	Transport        string `json:"transport"`
	Before           stats  `json:"before"`
	After            stats  `json:"after"`
	ExactEvents      int    `json:"exact_publications_received"`
	StopRequested    bool   `json:"stop_requested"`
	Restart          bool   `json:"ws_closed_and_new_ws_echo_after_stop"`
	ActiveObserved   uint64 `json:"admission_active_observed"`
	ClosedObserved   uint64 `json:"closed_sessions_observed"`
	AdmissionClosed  uint64 `json:"admission_sessions_completed"`
	BeforeObserved   bool   `json:"before_observed"`
	AfterObserved    bool   `json:"after_observed"`
	LastObserved     *stats `json:"last_observed,omitempty"`
	Failure          string `json:"failure,omitempty"`
}

func target(value string) (string, string, error) {
	u, err := url.Parse(value)
	if err != nil || u.Scheme != "http" || u.Hostname() == "" || u.User != nil ||
		(u.Path != "" && u.Path != "/") || u.RawQuery != "" || u.Fragment != "" {
		return "", "", errors.New("--url requires http://host[:port], without credentials or path")
	}
	port := u.Port()
	if port == "" {
		port = "80"
	}
	return "http://" + u.Host, net.JoinHostPort(u.Hostname(), port), nil
}

func (p *probe) request(method, path, auth, body string) (int, http.Header, []byte, error) {
	req, err := http.NewRequestWithContext(p.ctx, method, p.base+path, strings.NewReader(body))
	if err != nil {
		return 0, nil, nil, fmt.Errorf("HTTP %s %s construct: %w", method, path, err)
	}
	if auth != "" {
		req.Header.Set("Authorization", auth)
	}
	if method == "POST" {
		req.Header.Set("Content-Type", "application/octet-stream")
	}
	response, err := p.client.Do(req)
	if err != nil {
		return 0, nil, nil, fmt.Errorf("HTTP %s %s response headers: %w", method, path, err)
	}
	defer response.Body.Close()
	data, err := boundedBody(response.Body)
	if err != nil {
		err = fmt.Errorf("HTTP %s %s response body: %w", method, path, err)
	}
	return response.StatusCode, response.Header, data, err
}

func (p *probe) state() (stats, error) {
	status, _, data, err := p.request("GET", "/events/stats", "", "")
	if err != nil {
		return stats{}, err
	}
	if status != 200 {
		return stats{}, fmt.Errorf("stats status=%d", status)
	}
	value, err := parseStats(data)
	if err == nil {
		p.lastState = value
		p.haveState = true
	}
	return value, err
}

func (p *probe) health() error {
	status, _, body, err := p.request("GET", "/health", "", "")
	if err != nil {
		return err
	}
	if status != 200 || string(body) != `{"ok":true}` {
		return errors.New("minimal health mismatch")
	}
	return nil
}

func (p *probe) pause() error {
	select {
	case <-p.ctx.Done():
		return p.ctx.Err()
	case <-time.After(25 * time.Millisecond):
		return nil
	}
}

func earlier(first, second time.Time) time.Time {
	if !second.IsZero() && second.Before(first) {
		return second
	}
	return first
}

func (p *probe) deadline(interval time.Duration) time.Time {
	limit, _ := p.ctx.Deadline()
	return earlier(time.Now().Add(interval), limit)
}

func (p *probe) dial() (net.Conn, error) {
	connection, err := (&net.Dialer{Timeout: operationTimeout}).DialContext(p.ctx, "tcp", p.host)
	if err == nil {
		err = connection.SetDeadline(p.deadline(operationTimeout))
		if err != nil {
			connection.Close()
		}
	}
	return connection, err
}

func guardedResponse(connection net.Conn, request *http.Request) (*http.Response, *bufio.Reader, error) {
	reader := bufio.NewReaderSize(&headGuard{source: connection}, maxLineBytes)
	response, err := http.ReadResponse(reader, request)
	return response, reader, err
}

func (p *probe) open(path, auth string, lastID *string) (*stream, error) {
	connection, err := p.dial()
	if err != nil {
		return nil, fmt.Errorf("SSE %s dial: %w", path, err)
	}
	success := false
	defer func() {
		if !success {
			connection.Close()
		}
	}()
	request, err := http.NewRequest("GET", p.base+path, nil)
	if err != nil {
		return nil, fmt.Errorf("SSE %s construct: %w", path, err)
	}
	if auth != "" {
		request.Header.Set("Authorization", auth)
	}
	if lastID != nil {
		request.Header["Last-Event-Id"] = []string{*lastID}
	}
	if err = request.Write(connection); err != nil {
		return nil, fmt.Errorf("SSE %s write request: %w", path, err)
	}
	response, _, err := guardedResponse(connection, request)
	if err != nil {
		return nil, fmt.Errorf("SSE %s response headers: %w", path, err)
	}
	if response.StatusCode != 200 || response.Header.Get("Content-Type") != "text/event-stream" ||
		response.Header.Get("X-Sse-Test") != "shared" {
		return nil, fmt.Errorf("minimal SSE response mismatch: status=%d", response.StatusCode)
	}
	success = true
	limit, _ := p.ctx.Deadline()
	return &stream{conn: connection, response: response,
		reader: bufio.NewReaderSize(response.Body, maxLineBytes), limit: limit}, nil
}

func (s *stream) next() (event, error) {
	if err := s.conn.SetReadDeadline(earlier(time.Now().Add(operationTimeout), s.limit)); err != nil {
		return event{}, err
	}
	value, err := readEvent(s.reader, s.id)
	if err == nil {
		s.id = value.ID
	}
	return value, err
}

func (s *stream) connected(expected string) error {
	got, err := s.next()
	if err != nil {
		return err
	}
	if got != (event{"connected", expected, "boot:1", 1000}) {
		return errors.New("connected event/Last-Event-ID mismatch")
	}
	return nil
}

func (p *probe) publish(identifier, data string) error {
	deadline := time.Now().Add(operationTimeout)
	for {
		status, _, body, err := p.request("POST", "/events/publish?id="+url.QueryEscape(identifier), "", data)
		if err != nil {
			return err
		}
		if status == 200 && string(body) == "queued" {
			return nil // Queued is not delivered: callers must observe the event.
		}
		if status != 409 || string(body) != "busy" || !time.Now().Before(deadline) {
			return fmt.Errorf("publish was not queued: status=%d", status)
		}
		if err := p.pause(); err != nil {
			return err
		}
	}
}

func (p *probe) waitDrain(before stats, admitted uint64, repeatClose bool) (stats, error) {
	deadline := time.Now().Add(drainTimeout)
	for {
		if repeatClose {
			status, _, body, err := p.request("POST", "/events/close", "", "")
			if err != nil {
				return stats{}, err
			}
			if status != 200 || string(body) != "closing" {
				return stats{}, errors.New("close request mismatch")
			}
		}
		current, err := p.state()
		if err != nil {
			return stats{}, fmt.Errorf("drain stats: %w", err)
		}
		done, err := drained(before, current, admitted)
		if err != nil {
			return stats{}, fmt.Errorf("drain counters before=%+v current=%+v: %w", before, current, err)
		}
		if done {
			return current, nil
		}
		if !time.Now().Before(deadline) {
			return stats{}, fmt.Errorf("SSE did not drain: %+v", current)
		}
		if err := p.pause(); err != nil {
			return stats{}, err
		}
	}
}

func closedStream(s *stream, deadline time.Time) error {
	defer s.conn.Close()
	if err := s.conn.SetReadDeadline(earlier(deadline, s.limit)); err != nil {
		return err
	}
	// A finite queued tail may remain. Reaching this limit does not prove EOF.
	const maximum = 32768
	limited := &io.LimitedReader{R: s.reader, N: maximum + 1}
	_, err := io.Copy(io.Discard, limited)
	if limited.N == 0 {
		return errors.New("stream close tail exceeds bound")
	}
	if err != nil && !errors.Is(err, io.ErrUnexpectedEOF) && !errors.Is(err, syscall.ECONNRESET) {
		return fmt.Errorf("SSE stream did not close: %w", err)
	}
	return nil
}

func profileName(stop, admissionOnly bool) string {
	if admissionOnly {
		return "delivery-and-admission-only"
	}
	if stop {
		return "delivery-and-stop"
	}
	return "delivery"
}

func (p *probe) run(stop, admissionOnly bool, expectedBackend string, clients int) (report result, runError error) {
	report = result{URL: p.base, StopRequested: stop, Profile: profileName(stop, admissionOnly),
		Stage: "prepare", RequestedClients: clients}
	defer func() {
		// Only already-observed counters are copied here. No request is made after
		// failure/connection cleanup, which could allow a late queued admission.
		if p.haveState {
			last := p.lastState
			report.LastObserved = &last
		}
		if runError != nil {
			report.Status = "FAIL"
			report.Failure = runError.Error()
		}
	}()
	if !validClients(clients) {
		return report, errors.New("--clients must be between 1 and 4")
	}
	status, _, body, err := p.request("GET", "/http-test-config.json", "", "")
	if err != nil {
		return report, fmt.Errorf("prepare configuration: %w", err)
	}
	var configuration struct{ Transport string }
	if status != 200 || json.Unmarshal(body, &configuration) != nil || configuration.Transport == "" {
		return report, errors.New("prepare configuration: minimal frontend configuration missing")
	}
	report.Transport = configuration.Transport
	if expectedBackend != "" && ((expectedBackend == "tcp" && !strings.Contains(report.Transport, "shared TCP")) ||
		(expectedBackend == "httpd" && !strings.Contains(report.Transport, "ESP-IDF HTTPD"))) {
		return report, errors.New("configured transport does not match --backend")
	}
	if err = p.health(); err != nil {
		return report, fmt.Errorf("prepare health: %w", err)
	}
	report.Before, err = p.state()
	if err != nil {
		return report, fmt.Errorf("prepare initial stats: %w", err)
	}
	report.BeforeObserved = true
	if report.Before.Active != 0 || report.Before.Queued != 0 || report.Before.Begun != report.Before.Completed {
		return report, errors.New("probe requires no unrelated active/pending SSE clients")
	}
	for index, auth := range []string{"", "Basic dGVzdDp3cm9uZw=="} {
		status, headers, body, err := p.request("GET", "/events/private", auth, "")
		if err != nil {
			return report, fmt.Errorf("auth rejection case%d: %w", index+1, err)
		}
		if status != 401 || !strings.HasPrefix(headers.Get("WWW-Authenticate"), "Basic ") ||
			strings.Contains(string(body), "data:") {
			return report, fmt.Errorf("auth rejection case%d: private SSE did not reject before stream", index+1)
		}
	}
	unchanged, err := p.state()
	if err != nil {
		return report, fmt.Errorf("auth rejection stats: %w", err)
	}
	if unchanged != report.Before {
		return report, errors.New("auth rejection changed SSE counters")
	}
	previous, empty := "previous:7", ""
	current := report.Before
	for index, test := range []struct {
		name, path, auth, connected string
		last                        *string
	}{{"public-absent-id", "/events", "", "ready", nil},
		{"private-last-id", "/events/private", fixtureAuth, previous, &previous},
		{"public-empty-id", "/events", "", "", &empty}} {
		stage := fmt.Sprintf("case%d %s", index+1, test.name)
		report.Stage = stage
		s, err := p.open(test.path, test.auth, test.last)
		if err != nil {
			return report, fmt.Errorf("%s open: %w", stage, err)
		}
		defer s.conn.Close()
		if err = s.connected(test.connected); err != nil {
			return report, fmt.Errorf("%s connected event: %w", stage, err)
		}
		identifier := fmt.Sprintf("probe:%d", index)
		data := "Русский 中文 🙂\nline\x00tail\n"
		if err = p.publish(identifier, data); err != nil {
			return report, fmt.Errorf("%s publish: %w", stage, err)
		}
		got, err := s.next()
		if err != nil {
			return report, fmt.Errorf("%s receive publication: %w", stage, err)
		}
		if got != (event{"reading", data, identifier, 1000}) {
			return report, fmt.Errorf("%s publication fields mismatch: name=%q id=%q retry=%d bytes=%d expected_bytes=%d",
				stage, got.Name, got.ID, got.Retry, len(got.Data), len(data))
		}
		report.ExactEvents++
		current, err = p.waitDrain(current, 1, true)
		if err != nil {
			return report, fmt.Errorf("%s close/drain counters: %w", stage, err)
		}
		if err = closedStream(s, time.Now().Add(operationTimeout)); err != nil {
			return report, fmt.Errorf("%s closed stream: %w", stage, err)
		}
	}
	if stop {
		report.Stage = "stop"
		if err = p.stopStage(current, clients); err != nil {
			return report, fmt.Errorf("stop stage: %w", err)
		}
		report.Restart = true
		report.ActiveObserved = uint64(clients)
		report.AdmissionClosed = uint64(clients)
	}
	if admissionOnly {
		report.Stage = "admission-only"
		if err = p.admissionStage(current, clients); err != nil {
			return report, fmt.Errorf("admission-only stage: %w", err)
		}
		report.ActiveObserved = uint64(clients)
		report.AdmissionClosed = uint64(clients)
	}
	report.Stage = "final counters"
	report.After, err = p.state()
	if err != nil {
		return report, fmt.Errorf("final stats: %w", err)
	}
	report.AfterObserved = true
	admitted := uint64(3)
	if stop || admissionOnly {
		admitted += uint64(clients)
	}
	done, err := drained(report.Before, report.After, admitted)
	if err != nil || !done {
		return report, fmt.Errorf("final counters not drained: %v", err)
	}
	if err = p.health(); err != nil {
		return report, fmt.Errorf("final health: %w", err)
	}
	report.Status = "PASS"
	report.Stage = "complete"
	report.ClosedObserved = report.After.Completed - report.Before.Completed
	return report, nil
}

func main() {
	var address, output, backend string
	var stop, admissionOnly bool
	var clients int
	flag.StringVar(&address, "url", "", "current identified examples/minimal URL")
	flag.StringVar(&output, "out", "", "optional unique ignored JSON result path")
	flag.StringVar(&backend, "backend", "", "optional expected tcp or httpd transport")
	flag.BoolVar(&stop, "stop", false, "LAST stage: drain SSE and observe HTTP/WS restart; SSE remains stopped")
	flag.BoolVar(&admissionOnly, "admission-only", false, "DIAGNOSTIC: requested SSE clients without WS, then drain")
	flag.IntVar(&clients, "clients", 4, "stop/admission SSE client count 1..4; default4 needs isolated socket budget")
	flag.Parse()
	base, host, err := target(address)
	if err == nil && backend != "" && backend != "tcp" && backend != "httpd" {
		err = errors.New("--backend must be tcp or httpd")
	}
	if err == nil && stop && admissionOnly {
		err = errors.New("--stop and --admission-only are mutually exclusive")
	}
	if err == nil && !validClients(clients) {
		err = errors.New("--clients must be between 1 and 4")
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "MINIMAL_SSE_FAIL:", err)
		os.Exit(2)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	transport := &http.Transport{DisableKeepAlives: true, MaxConnsPerHost: 1,
		MaxResponseHeaderBytes: maxHeadBytes, ResponseHeaderTimeout: operationTimeout}
	defer transport.CloseIdleConnections()
	p := probe{ctx: ctx, base: base, host: host,
		client: &http.Client{Transport: transport, Timeout: operationTimeout,
			CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}
	report, runError := p.run(stop, admissionOnly, backend, clients)
	encoded, reportError := json.MarshalIndent(report, "", "  ")
	if reportError == nil && output != "" {
		reportError = os.WriteFile(output, append(encoded, '\n'), 0600)
	}
	if runError != nil {
		fmt.Fprintf(os.Stderr, "MINIMAL_SSE_FAIL profile=%s stage=%s: %v\n", report.Profile, report.Stage, runError)
		if reportError != nil {
			fmt.Fprintln(os.Stderr, "Failure report write:", reportError)
		}
		os.Exit(1)
	}
	if reportError != nil {
		fmt.Fprintln(os.Stderr, "MINIMAL_SSE_FAIL:", reportError)
		os.Exit(1)
	}
	fmt.Printf("MINIMAL_SSE_PASS\n%s\n", encoded)
}

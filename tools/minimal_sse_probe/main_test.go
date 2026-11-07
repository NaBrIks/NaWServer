package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

func TestEvents(t *testing.T) {
	wire := ": heartbeat\r\n\r\nevent: reading\nid: probe:1\nretry: 1000\ndata: Русский\ndata: a\x00b\ndata: \n\n"
	got, err := readEvent(bufio.NewReaderSize(strings.NewReader(wire), maxLineBytes), "previous")
	if err != nil || got != (event{"reading", "Русский\na\x00b\n", "probe:1", 1000}) {
		t.Fatalf("exact event: %#v %v", got, err)
	}
	got, err = readEvent(bufio.NewReader(strings.NewReader("id:\ndata:\n\n")), "previous")
	if err != nil || got.Data != "" || got.ID != "" {
		t.Fatalf("empty id/data: %#v %v", got, err)
	}
	got, err = readEvent(bufio.NewReader(strings.NewReader("data: x\n\n")), "inherited")
	if err != nil || got.ID != "inherited" {
		t.Fatal("id inheritance")
	}
}

func TestEventBoundsAndInvalidFixtures(t *testing.T) {
	for _, wire := range []string{
		"data: incomplete", "retry: +1\ndata: x\n\n", "retry: 4294967296\ndata: x\n\n",
		"retry:\ndata: x\n\n", "id: a\x00b\ndata: x\n\n", "data: \xff\n\n",
		"data: " + strings.Repeat("x", maxLineBytes) + "\n\n",
		strings.Repeat(": comment\n", maxEventLines+1),
		strings.Repeat("data: "+strings.Repeat("x", 900)+"\n", 5) + "\n",
	} {
		if _, err := readEvent(bufio.NewReaderSize(strings.NewReader(wire), maxLineBytes), ""); err == nil {
			t.Fatal("accepted invalid/unbounded event")
		}
	}
}

type oneByteReader struct{ io.Reader }

func (r oneByteReader) Read(destination []byte) (int, error) {
	if len(destination) > 1 {
		destination = destination[:1]
	}
	return r.Reader.Read(destination)
}

func TestHeaderGuardSplitsAndCoalescedBody(t *testing.T) {
	header := "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
	for _, input := range []io.Reader{strings.NewReader(header + strings.Repeat("b", maxHeadBytes)),
		oneByteReader{strings.NewReader(header + strings.Repeat("b", maxHeadBytes))}} {
		guard := &headGuard{source: input}
		got, err := io.ReadAll(guard)
		if err != nil || len(got) != len(header)+maxHeadBytes || guard.total != len(header) || !guard.done {
			t.Fatalf("body counted as head or split failed: total=%d err=%v", guard.total, err)
		}
	}
	for _, invalid := range []string{
		"HTTP/1.1 200 OK\n\n", "HTTP/1.1 200 OK\r\nX: " + strings.Repeat("x", maxLineBytes) + "\r\n\r\n",
		"HTTP/1.1 200 OK\r\n" + strings.Repeat("X: y\r\n", 33) + "\r\n",
		"HTTP/1.1 200 OK\r\n" + strings.Repeat("X: "+strings.Repeat("a", 900)+"\r\n", 10) + "\r\n",
	} {
		if _, err := io.ReadAll(&headGuard{source: strings.NewReader(invalid)}); err == nil {
			t.Fatal("accepted unbounded/malformed head")
		}
	}
}

func TestStatsAndDrainAreAuthoritative(t *testing.T) {
	for _, invalid := range []string{
		`{}`, `{"active":0,"begun":1,"completed":1}`, `{"active":0,"begun":0,"completed":1,"queued_bytes":0}`,
		`{"active":5,"begun":5,"completed":0,"queued_bytes":0}`,
		`{"active":0,"begun":1,"completed":1,"queued_bytes":0} {}`,
	} {
		if _, err := parseStats([]byte(invalid)); err == nil {
			t.Fatal("accepted missing/invalid counters")
		}
	}
	state, err := parseStats([]byte(`{"active":0,"begun":1,"completed":1,"queued_bytes":0,"rolled_back":0}`))
	if err != nil || state != (stats{0, 1, 1, 0}) {
		t.Fatal("valid stats rejected")
	}
	before := stats{0, 10, 10, 0}
	for _, pending := range []stats{{1, 11, 10, 0}, {0, 11, 10, 0}, {0, 11, 11, 1}} {
		if done, err := drained(before, pending, 1); err != nil || done {
			t.Fatal("acknowledgement/active0/queued tail treated as completion")
		}
	}
	if done, err := drained(before, stats{0, 11, 11, 0}, 1); err != nil || !done {
		t.Fatal("completed close not accepted")
	}
	if _, err := drained(before, stats{0, 12, 12, 0}, 1); err == nil {
		t.Fatal("unrelated admissions hidden")
	}
	if !activeBeforeStop(before, stats{4, 14, 10, 0}, 4) {
		t.Fatal("four active streams rejected")
	}
	for _, earlyClose := range []stats{{3, 14, 11, 0}, {0, 14, 14, 0}, {4, 15, 10, 0}} {
		if activeBeforeStop(before, earlyClose, 4) {
			t.Fatal("prematurely closed/unrelated stream counted as active across stop")
		}
	}
}

type fixtureTransport struct {
	status int
	body   string
}

func (f fixtureTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	return &http.Response{StatusCode: f.status, Header: make(http.Header),
		Body: io.NopCloser(strings.NewReader(f.body)), Request: request}, nil
}

func TestQueuedResponseDoesNotProveCompletion(t *testing.T) {
	p := probe{ctx: context.Background(), base: "http://fixture", client: &http.Client{
		Transport: fixtureTransport{200, "queued"}}}
	if err := p.publish("probe", "data"); err != nil {
		t.Fatal(err)
	}
	if done, err := drained(stats{}, stats{1, 1, 0, 7}, 1); err != nil || done {
		t.Fatal("queued response bypassed completion observation")
	}
	p.client.Transport = fixtureTransport{200, "wrong"}
	if err := p.publish("probe", "data"); err == nil {
		t.Fatal("invalid publication acknowledgement accepted")
	}
}

func TestBodiesTargetsAndWitness(t *testing.T) {
	if _, err := boundedBody(strings.NewReader(strings.Repeat("x", maxBodyBytes+1))); err == nil {
		t.Fatal("oversized body accepted")
	}
	if value, err := boundedBody(strings.NewReader("x\x00y")); err != nil || string(value) != "x\x00y" {
		t.Fatal("binary body changed")
	}
	for _, value := range []string{"", "https://device", "http://user:secret@device", "http://device/path"} {
		if _, _, err := target(value); err == nil || strings.Contains(err.Error(), "secret") {
			t.Fatal("invalid target accepted or credentials exposed")
		}
	}
	for _, value := range []string{"http://device", "http://[::1]:8080/"} {
		if _, _, err := target(value); err != nil {
			t.Fatal(err)
		}
	}
	for _, wire := range [][]byte{{0x01, 0}, {0x81, 0x80}, {0x81, 126}, {0x81, 2, 1}} {
		if _, _, err := witnessFrame(bytes.NewReader(wire)); err == nil {
			t.Fatal("malformed witness frame accepted")
		}
	}
	if op, body, err := witnessFrame(bytes.NewReader([]byte{0x81, 4, 't', 'i', 'c', 'k'})); err != nil || op != 1 || string(body) != "tick" {
		t.Fatal("valid witness frame rejected")
	}
	if !errors.Is(writeAll(zeroWriter{}, []byte{1}), io.ErrShortWrite) {
		t.Fatal("zero write accepted")
	}
}

func TestOverallDeadlineCapsRawSocketSteps(t *testing.T) {
	outer := time.Now().Add(time.Second)
	ctx, cancel := context.WithDeadline(context.Background(), outer)
	defer cancel()
	p := probe{ctx: ctx}
	if got := p.deadline(10 * time.Second); !got.Equal(outer) {
		t.Fatal("outer deadline lost")
	}
	first := outer.Add(-time.Second)
	if got := earlier(first, outer); !got.Equal(first) {
		t.Fatal("operation deadline extended")
	}
	if got := earlier(first, time.Time{}); !got.Equal(first) {
		t.Fatal("zero outer deadline changed bound")
	}
}

type failureReader struct{ failure error }

func (f failureReader) Read([]byte) (int, error) { return 0, f.failure }

type bodyFailureTransport struct{ failure error }

func (f bodyFailureTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	return &http.Response{StatusCode: 200, Header: make(http.Header),
		Body: io.NopCloser(failureReader{f.failure}), Request: request}, nil
}

func TestStageDiagnosticsPreserveOriginalFailure(t *testing.T) {
	failure := errors.New("fixture read failure")
	p := probe{ctx: context.Background(), base: "http://fixture", client: &http.Client{
		Transport: bodyFailureTransport{failure}}}
	_, _, _, err := p.request("POST", "/events/publish", "", "data")
	if !errors.Is(err, failure) || !strings.Contains(err.Error(), "HTTP POST /events/publish response body") {
		t.Fatal("HTTP operation or original failure lost")
	}
	report, err := p.run(false, false, "", 4)
	if !errors.Is(err, failure) || !strings.HasPrefix(err.Error(),
		"prepare configuration: HTTP GET /http-test-config.json response body") {
		t.Fatal("outer stage or original failure lost")
	}
	if report.Status != "FAIL" || report.Stage != "prepare" || report.Failure == "" ||
		report.BeforeObserved || report.AfterObserved || report.LastObserved != nil {
		t.Fatal("failure report invents observations or hides stage")
	}
}

func TestFailureReportUsesOnlyExistingStats(t *testing.T) {
	last := stats{0, 17, 17, 0}
	p := probe{ctx: context.Background(), base: "http://fixture", lastState: last, haveState: true,
		client: &http.Client{Transport: bodyFailureTransport{errors.New("fixture read failure")}}}
	report, err := p.run(true, false, "httpd", 2)
	if err == nil || report.LastObserved == nil || *report.LastObserved != last || report.AfterObserved ||
		report.RequestedClients != 2 || report.Stage != "prepare" {
		t.Fatal("existing snapshot lost or fresh counters invented")
	}
	if report.Restart || report.ClosedObserved != 0 {
		t.Fatal("failed probe claims restart/completion")
	}
}

func TestDiagnosticProfileHasNoRestartClaim(t *testing.T) {
	if profileName(false, true) != "delivery-and-admission-only" ||
		profileName(true, false) != "delivery-and-stop" || profileName(false, false) != "delivery" {
		t.Fatal("diagnostic and stop profiles are ambiguous")
	}
	report := result{Profile: profileName(false, true)}
	if report.StopRequested || report.Restart {
		t.Fatal("admission-only claims stop or restart")
	}
}

func TestRequestedClientBoundsAndExactCounters(t *testing.T) {
	before := stats{0, 10, 10, 0}
	for _, clients := range []int{1, 2, 4} {
		if !validClients(clients) || !activeBeforeStop(before,
			stats{uint64(clients), 10 + uint64(clients), 10, 0}, clients) {
			t.Fatal("valid requested count rejected")
		}
		if done, err := drained(before, stats{0, 10 + uint64(clients), 10 + uint64(clients), 0},
			uint64(clients)); err != nil || !done {
			t.Fatal("exact requested completions rejected")
		}
	}
	for _, clients := range []int{-1, 0, 5, 1000} {
		if validClients(clients) || activeBeforeStop(before, stats{4, 14, 10, 0}, clients) {
			t.Fatal("unbounded client count accepted")
		}
	}
	if activeBeforeStop(before, stats{2, 12, 10, 0}, 4) ||
		activeBeforeStop(before, stats{4, 14, 10, 0}, 2) {
		t.Fatal("two clients counted as four or unrelated clients hidden")
	}
}

type zeroWriter struct{}

func (zeroWriter) Write([]byte) (int, error) { return 0, nil }

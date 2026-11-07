package main

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

var base string
var checks int
var httpClient = &http.Client{Timeout: 4 * time.Second, Transport: &http.Transport{MaxIdleConnsPerHost: 1}}

const fixtureAuth = "Basic dGVzdDp0ZXN0LW9ubHk="

func require(ok bool, format string, args ...any) {
	checks++
	if !ok {
		panic(fmt.Sprintf(format, args...))
	}
}

type stats struct {
	Active, Pending, Begun, Completed, Rollback, Sent, Stall, Overflow, Heap int
	ProducerUS                                                               int `json:"producer_us"`
	StackMin                                                                 int `json:"stack_min"`
}
type publication struct{ Queued, Closed, Oversize, Overflow, Global, Busy int }
type event struct {
	Name, Data, ID string
	Retry          uint64
}
type stream struct {
	conn     net.Conn
	reader   *bufio.Reader
	response *http.Response
	id       string
}

func request(method, path string, auth bool, body string) (int, http.Header, []byte) {
	req, err := http.NewRequest(method, base+path, strings.NewReader(body))
	require(err == nil, "request: %v", err)
	if auth {
		req.Header.Set("Authorization", fixtureAuth)
	}
	resp, err := httpClient.Do(req)
	require(err == nil, "HTTP %s: %v", path, err)
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 16384))
	require(err == nil, "HTTP read %s: %v", path, err)
	return resp.StatusCode, resp.Header, data
}
func health() stats {
	code, _, body := request("GET", "/health", false, "")
	require(code == 200, "health status %d", code)
	var value stats
	require(json.Unmarshal(body, &value) == nil, "health JSON")
	return value
}
func waitActive(n int) stats {
	until := time.Now().Add(5 * time.Second)
	for {
		s := health()
		if s.Active == n {
			return s
		}
		require(time.Now().Before(until), "active=%d expected %d", s.Active, n)
		time.Sleep(20 * time.Millisecond)
	}
}
func closeAll(streams ...*stream) {
	until := time.Now().Add(2 * time.Second)
	for {
		code, _, _ := request("POST", "/control/close", true, "")
		if code == 200 {
			break
		}
		require(time.Now().Before(until), "close stayed busy")
		time.Sleep(5 * time.Millisecond)
	}
	for _, s := range streams {
		if s != nil {
			s.conn.Close()
		}
	}
	waitActive(0)
}
func dial() net.Conn {
	u, err := url.Parse(base)
	require(err == nil, "base URL")
	host := u.Host
	if !strings.Contains(host, ":") {
		host += ":80"
	}
	c, err := net.DialTimeout("tcp", host, 3*time.Second)
	require(err == nil, "dial: %v", err)
	if tcp, ok := c.(*net.TCPConn); ok {
		require(tcp.SetReadBuffer(1024) == nil, "set bounded test receive buffer")
	}
	c.SetDeadline(time.Now().Add(4 * time.Second))
	return c
}
func open(path string, id *string, auth bool, expected int) *stream {
	c := dial()
	req, _ := http.NewRequest("GET", base+path, nil)
	if id != nil {
		req.Header["Last-Event-Id"] = []string{*id}
	}
	if auth {
		req.Header.Set("Authorization", fixtureAuth)
	}
	require(req.Write(c) == nil, "write SSE request")
	r := bufio.NewReader(c)
	resp, err := http.ReadResponse(r, req)
	require(err == nil, "SSE response %s: %v", path, err)
	require(resp.StatusCode == expected, "SSE %s status=%d expected=%d", path, resp.StatusCode, expected)
	if expected != 200 {
		body, err := io.ReadAll(io.LimitReader(resp.Body, 4096))
		require(err == nil, "rejection body: %v", err)
		require(!bytes.Contains(body, []byte("data:")), "SSE bytes before rejection")
		c.Close()
		return nil
	}
	require(resp.Header.Get("Content-Type") == "text/event-stream", "SSE MIME")
	require(resp.Header.Get("X-Common") == "owned callback value", "copied header override")
	require(resp.Header.Get("X-SSE-Test") == "default", "default header")
	require(resp.Header.Get("Cache-Control") == "no-store", "SSE options cache")
	require(resp.Header.Get("X-Content-Type-Options") == "nosniff", "SSE options nosniff")
	s := &stream{conn: c, reader: bufio.NewReader(resp.Body), response: resp}
	if id != nil {
		s.id = *id
	}
	return s
}
func (s *stream) readEvent() event {
	s.conn.SetReadDeadline(time.Now().Add(4 * time.Second))
	out := event{Name: "message", ID: s.id}
	var data []string
	seen := false
	for {
		line, err := s.reader.ReadString('\n')
		require(err == nil, "SSE event read: %v", err)
		line = strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r")
		if line == "" {
			if !seen {
				continue
			}
			out.Data = strings.Join(data, "\n")
			s.id = out.ID
			return out
		}
		if strings.HasPrefix(line, ":") {
			continue
		}
		key, value, found := strings.Cut(line, ":")
		if !found {
			value = ""
		}
		value = strings.TrimPrefix(value, " ")
		switch key {
		case "data":
			seen = true
			data = append(data, value)
		case "event":
			if value != "" {
				out.Name = value
			}
		case "id":
			out.ID = value
		case "retry":
			out.Retry, _ = strconv.ParseUint(value, 10, 32)
		}
	}
}
func connected(s *stream, want string) {
	e := s.readEvent()
	require(e.Name == "connected" && e.Data == want, "initial Last-Event-ID event=%#v", e)
}
func publish(query string) publication {
	code, _, body := request("POST", "/control/publish?"+query, true, "")
	require(code == 200, "publish status %d", code)
	var p publication
	require(json.Unmarshal(body, &p) == nil, "publish JSON")
	return p
}
func publishOne(query string) publication {
	until := time.Now().Add(time.Second)
	for {
		p := publish(query)
		if p.Busy == 0 {
			return p
		}
		require(p.Queued == 0, "partial busy broadcast requires explicit application recovery")
		require(time.Now().Before(until), "publish remained busy")
		time.Sleep(2 * time.Millisecond)
	}
}
func websocket() {
	c := dial()
	defer c.Close()
	u, _ := url.Parse(base)
	fmt.Fprintf(c, "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n", u.Host, base64.StdEncoding.EncodeToString([]byte("0123456789abcdef")))
	r := bufio.NewReader(c)
	resp, err := http.ReadResponse(r, &http.Request{Method: "GET"})
	require(err == nil && resp.StatusCode == 101, "WS handshake: %v", err)
	payload := []byte("alongside SSE")
	wire := []byte{0x81, 0x80 | byte(len(payload)), 1, 2, 3, 4}
	for i, b := range payload {
		wire = append(wire, b^wire[2+i%4])
	}
	_, err = c.Write(wire)
	require(err == nil, "WS write")
	head := make([]byte, 2)
	_, err = io.ReadFull(r, head)
	require(err == nil && head[0] == 0x81 && int(head[1]) == len(payload), "WS reply header %v %v", head, err)
	got := make([]byte, len(payload))
	_, err = io.ReadFull(r, got)
	require(err == nil && bytes.Equal(got, payload), "WS echo")
	// Graceful client close, masked empty CLOSE frame.
	c.Write([]byte{0x88, 0x80, 1, 2, 3, 4})
}
func asset() {
	code, _, body := request("GET", "/asset", false, "")
	require(code == 200 && len(body) == 4096, "static asset response")
	require(bytes.Equal(body, make([]byte, 4096)), "static asset binary bytes")
}
func abortRoute(path string) {
	before := health()
	c := dial()
	req, _ := http.NewRequest("GET", base+path, nil)
	req.Header.Set("Authorization", fixtureAuth)
	require(req.Write(c) == nil, "abort request")
	raw, err := io.ReadAll(io.LimitReader(c, 4096))
	c.Close()
	require(err == nil || errors.Is(err, io.EOF) || strings.Contains(err.Error(), "reset"), "abort did not close: %v", err)
	require(!bytes.Contains(raw, []byte("500 Internal")) && !bytes.Contains(raw, []byte("must never appear")), "second ordinary response after handoff")
	after := waitActive(0)
	require(after.Begun == before.Begun+1 && after.Completed == before.Completed+1, "abort completion")
}
func run() map[string]any {
	waitActive(0)
	before := health()
	open("/events", nil, false, 401)
	open("/events/forbidden", nil, true, 403)
	require(health().Begun == before.Begun, "auth rejection reserved an SSE request")
	bad := strings.Repeat("a", 129)
	open("/events", &bad, true, 400)
	code, _, body := request("GET", "/events", true, "x")
	require(code == 400 && !bytes.Contains(body, []byte("data:")), "GET body admission")
	code, _, body = request("GET", "/events/rollback", true, "")
	require(code == 409 && string(body) == "reservation rolled back", "real native rollback")
	require(health().Rollback == before.Rollback+1, "rollback accounting")
	for _, path := range []string{"/events/error", "/events/duplicate"} {
		abortRoute(path)
	}
	last := "previous:7"
	a := open("/events", &last, true, 200)
	connected(a, last)
	empty := ""
	b := open("/events/context", &empty, true, 200)
	connected(b, "")
	c := open("/events", nil, true, 200)
	connected(c, "<absent>")
	d := open("/events", nil, true, 200)
	connected(d, "<absent>")
	open("/events", nil, true, 503)
	require(health().Active == 4, "fifth client changed active set")
	start := time.Now()
	asset()
	websocket()
	mixedMS := time.Since(start).Milliseconds()
	for _, tc := range []struct {
		kind, data, id string
		retry          uint64
	}{{"utf8", "Русский 中文 日本語 🙂\nline\n", "boot:42", 500}, {"empty", "", "", 0}, {"nul", "a\x00b", "", 0}} {
		p := publishOne("case=" + tc.kind)
		require(p.Queued == 4, "four-client publish: %#v", p)
		for _, s := range []*stream{a, b, c, d} {
			e := s.readEvent()
			require(e.Name == "reading" && e.Data == tc.data && e.ID == tc.id && e.Retry == tc.retry, "event %s mismatch %#v", tc.kind, e)
		}
	}
	// Fill the common payload budget in one HTTPD callback, without attributing it
	// to an individual client. Queued is deliberately not treated as delivered.
	p := publishOne("case=large&burst=3")
	require(p.Global > 0 && p.Overflow == 0 && health().Active == 4, "global budget result %#v", p)
	closeAll(a, b, c, d)
	heaps := []int{}
	for round := 0; round < 3; round++ {
		for i := 0; i < 10; i++ {
			s := open("/events", nil, true, 200)
			connected(s, "<absent>")
			closeAll(s)
		}
		time.Sleep(100 * time.Millisecond)
		heaps = append(heaps, health().Heap)
	}
	slow := open("/events", nil, true, 200)
	connected(slow, "<absent>")
	healthy := open("/events", nil, true, 200)
	connected(healthy, "<absent>")
	h2 := open("/events", nil, true, 200)
	connected(h2, "<absent>")
	baseline := health()
	until := time.Now().Add(20 * time.Second)
	queued := 0
	mixed := false
	var pendingSince time.Time
	for {
		st := health()
		if st.Stall > baseline.Stall {
			break
		}
		require(time.Now().Before(until), "slow reader did not stall")
		if st.Pending == 0 {
			p = publishOne("case=large&slot=0")
			require(p.Overflow == 0 && p.Global == 0, "stall substituted by overflow")
			queued += p.Queued
			pendingSince = time.Time{}
		} else {
			if pendingSince.IsZero() {
				pendingSince = time.Now()
			}
			if !mixed && time.Since(pendingSince) > 100*time.Millisecond {
				asset()
				websocket()
				p = publishOne("case=utf8&slot=1")
				require(p.Queued == 1, "healthy 1 publish")
				e := healthy.readEvent()
				require(e.Data == "Русский 中文 日本語 🙂\nline\n", "healthy 1 during stall")
				p = publishOne("case=utf8&slot=2")
				require(p.Queued == 1, "healthy 2 publish")
				require(h2.readEvent().Name == "reading", "healthy 2 during stall")
				mixed = true
			}
		}
		time.Sleep(5 * time.Millisecond)
	}
	require(mixed, "mixed traffic never checked during pending stall")
	st := waitActive(2)
	require(st.Overflow == baseline.Overflow, "stall closed by overflow")
	closeAll(slow, healthy, h2)
	// Explicit individual queue overflow is a separate test.
	s := open("/events", nil, true, 200)
	connected(s, "<absent>")
	baseline = health()
	until = time.Now().Add(3 * time.Second)
	for {
		p = publish("case=large&slot=0&burst=4")
		if p.Overflow > 0 {
			break
		}
		require(time.Now().Before(until), "individual overflow not reached")
		time.Sleep(5 * time.Millisecond)
	}
	waitActive(0)
	s.conn.Close()
	require(health().Overflow > baseline.Overflow, "overflow counter")
	final := health()
	require(final.Begun == final.Completed && final.Pending == 0, "outstanding async requests after smoke")
	return map[string]any{"checks": checks, "begun": final.Begun, "completed": final.Completed, "rolled_back": final.Rollback, "stall": final.Stall, "overflow": final.Overflow, "slow_frames_queued": queued, "mixed_http_ws_ms": mixedMS, "producer_max_us": final.ProducerUS, "churn_free_heap": heaps, "http_stack_min_bytes": final.StackMin}
}
func stopWithActive() {
	var list []*stream
	for i := 0; i < 4; i++ {
		s := open("/events", nil, true, 200)
		connected(s, "<absent>")
		list = append(list, s)
	}
	publish("case=large")
	code, _, _ := request("POST", "/control/stop", true, "")
	require(code == 200, "stop request")
	for _, s := range list {
		s.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		_, err := io.ReadAll(io.LimitReader(s.reader, 16384))
		s.conn.Close()
		require(err == nil || strings.Contains(err.Error(), "reset"), "shutdown left stream open: %v", err)
	}
	fmt.Printf("SSE_LAN_STOP checks=%d\n", checks)
}
func main() {
	var output string
	var stopOnly bool
	flag.StringVar(&base, "url", "", "identified current C3 HTTP URL")
	flag.StringVar(&output, "out", "", "numeric result JSON path")
	flag.BoolVar(&stopOnly, "stop-only", false, "drain/stop with four active SSE requests after browser check")
	flag.Parse()
	require(base != "", "-url required")
	base = strings.TrimRight(base, "/")
	defer func() {
		if failure := recover(); failure != nil {
			fmt.Fprintln(os.Stderr, "SSE_LAN_FAIL:", failure)
			os.Exit(1)
		}
	}()
	if stopOnly {
		stopWithActive()
		return
	}
	result := run()
	result["checks"] = checks
	data, err := json.MarshalIndent(result, "", "  ")
	require(err == nil, "result JSON")
	if output != "" {
		require(os.WriteFile(output, append(data, '\n'), 0600) == nil, "write numeric results")
	}
	fmt.Printf("SSE_LAN_PASS checks=%d\n%s\n", checks, data)
}

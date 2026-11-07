package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

func TestHTTPContract(t *testing.T) {
	d := &demo{started: time.Now(), profile: "balanced"}
	server := httptest.NewServer(d.handler())
	defer server.Close()
	cases := []struct {
		method, path, body, user, password, origin string
		status                                     int
		contains                                   string
	}{
		{"GET", "/health", "", "", "", "", 200, `"mode":"local"`},
		{"GET", "/api/telemetry", "", "", "", "", 200, `"sensor_synthetic":true`},
		{"GET", "/api/echo?value=%D0%9F%D1%80%D0%B8%D0%B2%D0%B5%D1%82%20%26%20%2B", "", "", "", "", 200, "Привет & +"},
		{"POST", "/api/echo", "value=hello%00world", "", "", "", 200, "hello\x00world"},
		{"POST", "/api/control", "profile=quiet", "", "", "", 401, "unauthorized"},
		{"POST", "/api/control", "profile=quiet", "demo", "wrong", "", 401, "unauthorized"},
		{"POST", "/api/control", "profile=quiet", "demo", "showcase-only", "", 200, `"profile":"quiet"`},
		{"POST", "/api/control", "profile=invalid", "demo", "showcase-only", "", 400, "invalid profile"},
		{"POST", "/api/control", "profile=quiet", "demo", "showcase-only", "http://foreign.invalid", 403, "origin rejected"},
		{"GET", "/api/control", "", "demo", "showcase-only", "", 405, "POST required"},
		{"POST", "/api/echo", "value=" + strings.Repeat("a", 4097), "", "", "", 400, "body rejected"},
		{"GET", "/missing", "", "", "", "", 404, "route not found"},
		{"GET", "/api/stream", "", "", "", "", 200, "Not SSE"},
		{"POST", "/api/telemetry", "", "", "", "", 405, "GET required"},
	}
	for i, c := range cases {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			req, _ := http.NewRequest(c.method, server.URL+c.path, strings.NewReader(c.body))
			if c.method == "POST" {
				req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			}
			if c.user != "" {
				req.SetBasicAuth(c.user, c.password)
			}
			req.Header.Set("Origin", c.origin)
			res, err := server.Client().Do(req)
			if err != nil {
				t.Fatal(err)
			}
			defer res.Body.Close()
			data, _ := io.ReadAll(res.Body)
			if res.StatusCode != c.status || !strings.Contains(string(data), c.contains) {
				t.Fatalf("%d %q", res.StatusCode, data)
			}
			if res.Header.Get("X-Content-Type-Options") != "nosniff" {
				t.Fatal("missing safety header")
			}
			if c.status == 401 && res.Header.Get("WWW-Authenticate") == "" {
				t.Fatal("missing challenge")
			}
		})
	}
	for path, mime := range map[string]string{"/": "text/html", "/app.js": "application/javascript", "/app.css": "text/css"} {
		res, err := http.Get(server.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		_ = res.Body.Close()
		if res.StatusCode != 200 || !strings.HasPrefix(res.Header.Get("Content-Type"), mime) {
			t.Fatalf("asset %s: %s", path, res.Header)
		}
	}
	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	res, err := client.Get(server.URL + "/go")
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	if res.StatusCode != 302 || res.Header.Get("Location") != "/#capabilities" {
		t.Fatal("redirect")
	}
}

func masked(op byte, data []byte) []byte {
	frame := []byte{0x80 | op}
	n := len(data)
	if n < 126 {
		frame = append(frame, 0x80|byte(n))
	} else {
		frame = append(frame, 0x80|126, byte(n>>8), byte(n))
	}
	mask := []byte{0x12, 0x34, 0x56, 0x78}
	frame = append(frame, mask...)
	for i, b := range data {
		frame = append(frame, b^mask[i%4])
	}
	return frame
}
func serverFrame(t *testing.T, r *bufio.Reader) (byte, []byte) {
	t.Helper()
	a, err := r.ReadByte()
	if err != nil {
		t.Fatal(err)
	}
	b, err := r.ReadByte()
	if err != nil {
		t.Fatal(err)
	}
	if b&128 != 0 {
		t.Fatal("masked server frame")
	}
	n := int(b & 127)
	if n == 126 {
		var v uint16
		if err = binary.Read(r, binary.BigEndian, &v); err != nil {
			t.Fatal(err)
		}
		n = int(v)
	}
	data := make([]byte, n)
	if _, err = io.ReadFull(r, data); err != nil {
		t.Fatal(err)
	}
	return a & 15, data
}
func TestWebSocketRoundTrip(t *testing.T) {
	d := &demo{started: time.Now()}
	s := httptest.NewServer(d.handler())
	defer s.Close()
	u, _ := url.Parse(s.URL)
	conn, err := net.DialTimeout("tcp", u.Host, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(10 * time.Second))
	_, _ = fmt.Fprintf(conn, "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\n\r\n", u.Host, base64.StdEncoding.EncodeToString([]byte("0123456789abcdef")), s.URL)
	reader := bufio.NewReader(conn)
	res, err := http.ReadResponse(reader, nil)
	if err != nil || res.StatusCode != 101 {
		t.Fatalf("handshake %v %v", res, err)
	}
	for _, op := range []byte{1, 2} {
		for _, n := range []int{0, 1, 127, 128, 256, 4096} {
			payload := bytes.Repeat([]byte{'x'}, n)
			if op == 2 {
				for i := range payload {
					payload[i] = byte(i % 256)
				}
			}
			_, _ = conn.Write(masked(op, payload))
			kind, actual := serverFrame(t, reader)
			if kind != op || !bytes.Equal(actual, payload) {
				t.Fatalf("echo op=%d n=%d", op, n)
			}
		}
	}
	// Wait once for the periodic broadcast, independent from echo responses.
	op, body := serverFrame(t, reader)
	if op != 1 || !bytes.Contains(body, []byte(`"event":"tick"`)) {
		t.Fatalf("tick %d %q", op, body)
	}
	_, _ = conn.Write(masked(9, []byte("ping")))
	op, body = serverFrame(t, reader)
	if op != 10 || string(body) != "ping" {
		t.Fatal("ping/pong")
	}
	_, _ = conn.Write(masked(2, bytes.Repeat([]byte{'x'}, 4097)))
	op, body = serverFrame(t, reader)
	if op != 8 || binary.BigEndian.Uint16(body) != 1009 {
		t.Fatal("oversized close")
	}
	if d.echoes.Load() != 12 {
		t.Fatalf("echo count %d", d.echoes.Load())
	}
}
func TestFrameRejections(t *testing.T) {
	for _, c := range []struct {
		data []byte
		code uint16
	}{{[]byte{0x81, 0}, 1002}, {[]byte{0x01, 0x80}, 1003}, {masked(1, []byte{0xff}), 1007}, {masked(8, []byte{1}), 1002}} {
		_, _, code, err := readFrame(bufio.NewReader(bytes.NewReader(c.data)))
		if err == nil || code != c.code {
			t.Fatalf("code %d wanted %d", code, c.code)
		}
	}
}
func TestPackerRoundTrip(t *testing.T) {
	p := filepath.Join(t.TempDir(), "assets.hpp")
	if err := packAssets(p); err != nil {
		t.Fatal(err)
	}
	first, _ := os.ReadFile(p)
	if err := packAssets(p); err != nil {
		t.Fatal(err)
	}
	second, _ := os.ReadFile(p)
	if !bytes.Equal(first, second) {
		t.Fatal("packer not deterministic")
	}
	text := string(first)
	for _, name := range []string{"index.html", "app.css", "app.js"} {
		id := strings.ReplaceAll(name, ".", "_")
		start := strings.Index(text, id+"[] = {\n")
		if start < 0 {
			t.Fatal(name)
		}
		block := text[start+len(id+"[] = {\n"):]
		block = block[:strings.Index(block, "};")]
		var packed []byte
		for _, entry := range strings.Split(strings.TrimSpace(block), ",") {
			entry = strings.TrimSpace(entry)
			if entry == "" {
				continue
			}
			n, err := strconv.ParseUint(entry, 0, 8)
			if err != nil {
				t.Fatal(err)
			}
			packed = append(packed, byte(n))
		}
		reader, err := gzip.NewReader(bytes.NewReader(packed))
		if err != nil {
			t.Fatal(err)
		}
		actual, err := io.ReadAll(reader)
		_ = reader.Close()
		if err != nil {
			t.Fatal(err)
		}
		expected, _ := assets.ReadFile("web/" + name)
		if !bytes.Equal(actual, expected) {
			t.Fatal("asset mismatch " + name)
		}
	}
}
func TestTelemetryJSON(t *testing.T) {
	d := &demo{started: time.Now()}
	w := httptest.NewRecorder()
	d.handler().ServeHTTP(w, httptest.NewRequest("GET", "/api/telemetry", nil))
	var result map[string]any
	if json.Unmarshal(w.Body.Bytes(), &result) != nil || result["mode"] != "local" {
		t.Fatal("telemetry contract")
	}
}

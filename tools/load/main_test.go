package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

func TestOwnedWebSocketCancellationDrainsWithoutFailureOrReconnect(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	stats := newCounters(1)
	manager := newConnectionManager()
	client, peer := net.Pipe()
	defer peer.Close()
	manager.add(client)

	done := make(chan struct{})
	var reconnects uint64
	go func() {
		defer close(done)
		_, err := readWSFrame(bufio.NewReader(client))
		if shouldReportNetworkFailure(ctx, err) {
			stats.failure(0, err)
			reconnects++
		}
		manager.remove(client)
		_ = client.Close()
	}()

	cancel()
	manager.closeAll()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("WebSocket reader did not drain after owned cancellation")
	}
	if got := stats.snapshot().Errors; got != 0 {
		t.Fatalf("owned cancellation counted as %d network errors", got)
	}
	if reconnects != 0 {
		t.Fatalf("owned cancellation caused %d reconnects", reconnects)
	}
	manager.mu.Lock()
	remaining := len(manager.all)
	manager.mu.Unlock()
	if remaining != 0 {
		t.Fatalf("connection manager retained %d sockets after cancellation", remaining)
	}
}

func TestCBORDecodeAndBootIdentity(t *testing.T) {
	info := cborMap(cborText("v"), cborUint(2), cborText("boot_id"), cborText("c6-test-boot"))
	if got := cborBootID(info); got != "c6-test-boot" {
		t.Fatalf("boot ID = %q", got)
	}
	if _, err := decodeCBOR(append(info, 0)); err == nil {
		t.Fatal("trailing CBOR byte accepted")
	}
	if _, err := decodeCBOR([]byte{0x5a, 0xff}); err == nil {
		t.Fatal("truncated CBOR bytes accepted")
	}
}

func TestCapturedC6SummaryIfAvailable(t *testing.T) {
	data, err := os.ReadFile("../../local/night26/rapid-smoke-c6-summary.cbor")
	if errors.Is(err, os.ErrNotExist) {
		t.Skip("local C6 capture is not present")
	}
	if err != nil {
		t.Fatal(err)
	}
	value, err := decodeCBOR(data)
	if err != nil {
		t.Fatalf("decode captured C6 summary: %v", err)
	}
	root, ok := value.(map[any]any)
	if !ok || root["source"] != "device" || root["boot_id"] == nil {
		t.Fatalf("unexpected captured summary envelope: %#v", value)
	}
}

func TestCapturedC6WebSocketSummaryIfAvailable(t *testing.T) {
	data, err := os.ReadFile("../../local/night26/c6-ws-telemetry.cbor")
	if errors.Is(err, os.ErrNotExist) {
		t.Skip("local C6 WebSocket capture is not present")
	}
	if err != nil {
		t.Fatal(err)
	}
	kind, err := wsUnsolicitedKind(data)
	if err != nil || kind != "summary" {
		t.Fatalf("decode captured C6 WebSocket summary: kind=%q err=%v", kind, err)
	}
}

func TestCBORSummaryFloatNullAndBoolValues(t *testing.T) {
	array := append(cborHead(4, 1), cborFloat32(12.5)...)
	data := cborMap(cborText("core_busy_pct"), array, cborText("rssi_dbm"), []byte{0xf6}, cborText("connected"), []byte{0xf5})
	encoded := cborMap(cborText("v"), cborUint(2), cborText("source"), cborText("device"), cborText("boot_id"), cborText("fixture"), cborText("seq"), cborUint(7), cborText("captured_ms"), cborUint(9), cborText("data"), data)
	decoded, err := decodeCBOR(encoded)
	if err != nil {
		t.Fatal(err)
	}
	root := decoded.(map[any]any)
	inner := root["data"].(map[any]any)
	if got := inner["core_busy_pct"].([]any)[0]; got != float64(12.5) {
		t.Fatalf("float decode=%v", got)
	}
	if inner["rssi_dbm"] != nil || inner["connected"] != true {
		t.Fatalf("null/bool decode=%#v", inner)
	}
}

func cborFloat32(value float32) []byte {
	result := []byte{0xfa, 0, 0, 0, 0}
	binary.BigEndian.PutUint32(result[1:], math.Float32bits(value))
	return result
}

func TestValidateOptionsAndResumeCursor(t *testing.T) {
	options, err := validateOptions("http://192.0.2.70", time.Minute, 4, 250*time.Millisecond, "192.0.2.40", "", "/", "reload")
	if err != nil || options.clients != 4 {
		t.Fatalf("valid options rejected: %+v %v", options, err)
	}
	for _, input := range []struct {
		base    string
		clients int
	}{{"https://192.0.2.70", 2}, {"http://192.0.2.70/path", 2}, {"http://192.0.2.70", 3}, {"http://192.0.2.70:443", 2}} {
		if _, err := validateOptions(input.base, time.Minute, input.clients, 250*time.Millisecond, "192.0.2.40", "", "/", "reload"); err == nil {
			t.Fatalf("invalid options accepted: %+v", input)
		}
	}
	if _, err := validateOptions("http://192.0.2.70", time.Minute, 2, 250*time.Millisecond, "192.0.2.40", "", "/", "unknown"); err == nil {
		t.Fatal("invalid asset mode accepted")
	}
	directory := t.TempDir()
	path := filepath.Join(directory, "load.jsonl")
	prior := map[string]any{"base": "http://192.0.2.70", "counters": map[string]any{"sse_last_event_id": "boot:41"}}
	data, _ := json.Marshal(prior)
	if err := os.WriteFile(path, append(data, '\n'), 0600); err != nil {
		t.Fatal(err)
	}
	cursor, err := resumeCursor(path, "http://192.0.2.70")
	if err != nil || cursor != "boot:41" {
		t.Fatalf("resume cursor=%q err=%v", cursor, err)
	}
	if _, err := resumeCursor(path, "http://192.0.2.71"); err == nil {
		t.Fatal("accepted output from a different device origin")
	}
}

func TestCachedAssetModePrefetchesOnceThenUsesSummary(t *testing.T) {
	var jsRequests, cssRequests, summaryRequests int
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/assets/app-A1.js":
			jsRequests++
			_, _ = writer.Write([]byte("javascript bundle"))
		case "/assets/app-B2.css":
			cssRequests++
			_, _ = writer.Write([]byte("css bundle"))
		case "/api/v2/summary":
			summaryRequests++
			_, _ = writer.Write([]byte("summary"))
		default:
			http.NotFound(writer, request)
		}
	}))
	defer server.Close()

	paths := []string{"/assets/app-A1.js", "/assets/app-B2.css"}
	stats := newCounters(2)
	client := server.Client()
	if err := prefetchStaticAssets(context.Background(), client, server.URL, paths, stats); err != nil {
		t.Fatal(err)
	}
	options := runOptions{base: server.URL, assetPaths: paths, assetMode: "cached"}
	for cycle := 0; cycle < 4; cycle++ {
		if err := doHTTPWork(context.Background(), options, client, stats, 0, cycle); err != nil {
			t.Fatal(err)
		}
	}
	if jsRequests != 1 || cssRequests != 1 {
		t.Fatalf("asset request counts JS=%d CSS=%d, want one prefetch each", jsRequests, cssRequests)
	}
	if summaryRequests != 4 {
		t.Fatalf("summary request count=%d, want one per cached asset-cycle slot", summaryRequests)
	}
	snapshot := stats.snapshot()
	if snapshot.Assets != 2 || snapshot.Summary != 4 {
		t.Fatalf("unexpected counted requests: assets=%d summary=%d", snapshot.Assets, snapshot.Summary)
	}
}

func TestStopFileRequested(t *testing.T) {
	directory := t.TempDir()
	path := filepath.Join(directory, "stop.load")
	if stopFileRequested("") || stopFileRequested(path) {
		t.Fatal("missing stop file requested cancellation")
	}
	if err := os.WriteFile(path, []byte("stop"), 0600); err != nil {
		t.Fatal(err)
	}
	if !stopFileRequested(path) {
		t.Fatal("existing stop file did not request cancellation")
	}
}

func TestFailureReasonsAreCapped(t *testing.T) {
	stats := newCounters(2)
	for index := 0; index < 20; index++ {
		stats.failure(0, errors.New(fmt.Sprintf("failure-%d", index)))
	}
	snapshot := stats.snapshot()
	if len(snapshot.ErrorReasons) != 13 || snapshot.ErrorReasons["other_capped"] != 8 || snapshot.LastError != "failure-19" {
		t.Fatalf("error diagnostics not bounded: reasons=%d capped=%d last=%q", len(snapshot.ErrorReasons), snapshot.ErrorReasons["other_capped"], snapshot.LastError)
	}
}

func TestReadSSEProgress(t *testing.T) {
	reader := bufio.NewReader(bytes.NewBufferString(": heartbeat\nid: boot:42\nevent: diagnostic\ndata: first\ndata: second\n\n"))
	id, data, err := readSSEEvent(reader)
	if err != nil || id != "boot:42" || data != "first\nsecond" {
		t.Fatalf("SSE id=%q data=%q err=%v", id, data, err)
	}
	if _, _, err := readSSEEvent(bufio.NewReader(bytes.NewBufferString("id: " + string(bytes.Repeat([]byte{'x'}, 4200)) + "\n\n"))); err == nil {
		t.Fatal("oversized SSE line accepted")
	}
}

func TestWebSocketHandshakeKeyAndAccept(t *testing.T) {
	key := base64.StdEncoding.EncodeToString([]byte("0123456789abcdef"))
	accept, err := expectedWebSocketAccept(key)
	if err != nil {
		t.Fatal(err)
	}
	if err := validateWebSocketHandshake(key, accept); err != nil {
		t.Fatal(err)
	}
	wrongLength := base64.StdEncoding.EncodeToString([]byte("load-harness-ws-key"))
	if _, err := expectedWebSocketAccept(wrongLength); err == nil {
		t.Fatal("accepted a non-16-byte decoded key")
	}
	if err := validateWebSocketHandshake(key, "wrong-accept"); err == nil {
		t.Fatal("accepted incorrect server accept value")
	}
}

func TestExtractHashedStaticAssets(t *testing.T) {
	document := `<html><link href="/assets/app-A1.css" rel="stylesheet"><script src="/assets/app-B2.js"></script><img src="/logo.png"><script src="/assets/../private.js"></script></html>`
	paths := extractAssetPaths(document, "/")
	want := []string{"/assets/app-A1.css", "/assets/app-B2.js"}
	if !reflect.DeepEqual(paths, want) {
		t.Fatalf("asset paths=%v want %v", paths, want)
	}
	paths = extractAssetPaths(document, "/extra.css")
	if !reflect.DeepEqual(paths, []string{"/assets/app-A1.css", "/assets/app-B2.js", "/extra.css"}) {
		t.Fatalf("explicit path not added: %v", paths)
	}
}

func TestBoundedGzipBody(t *testing.T) {
	var compressed bytes.Buffer
	writer := gzip.NewWriter(&compressed)
	if _, err := writer.Write([]byte("index-with-hashed-assets")); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	decoded, wireBytes, encoding, err := readHTTPBody(bytes.NewReader(compressed.Bytes()), "gzip", 1024, 1024)
	if err != nil || string(decoded) != "index-with-hashed-assets" || wireBytes != uint64(compressed.Len()) || encoding != "gzip" {
		t.Fatalf("decoded=%q wire=%d encoding=%q err=%v", decoded, wireBytes, encoding, err)
	}
	if _, _, _, err := readHTTPBody(bytes.NewReader(compressed.Bytes()), "gzip", int64(compressed.Len()-1), 1024); err == nil {
		t.Fatal("wire-size cap was not enforced")
	}
	var expanded bytes.Buffer
	writer = gzip.NewWriter(&expanded)
	if _, err := writer.Write(bytes.Repeat([]byte{'x'}, 100)); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := readHTTPBody(bytes.NewReader(expanded.Bytes()), "gzip", 1024, 99); err == nil {
		t.Fatal("decoded-size cap was not enforced")
	}
}

func TestWebSocketBinaryCBORExactEcho(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()
	requestPayload := echoCBOR("client-0-1", "Русский🙂")
	serverDone := make(chan error, 1)
	go func() {
		reader := bufio.NewReader(server)
		header := make([]byte, 2)
		if _, err := io.ReadFull(reader, header); err != nil {
			serverDone <- err
			return
		}
		length := int(header[1] & 0x7f)
		if length == 126 {
			extended := make([]byte, 2)
			if _, err := io.ReadFull(reader, extended); err != nil {
				serverDone <- err
				return
			}
			length = int(extended[0])<<8 | int(extended[1])
		}
		if header[0] != 0x82 || header[1]&0x80 == 0 {
			serverDone <- io.ErrUnexpectedEOF
			return
		}
		mask := make([]byte, 4)
		if _, err := io.ReadFull(reader, mask); err != nil {
			serverDone <- err
			return
		}
		payload := make([]byte, length)
		if _, err := io.ReadFull(reader, payload); err != nil {
			serverDone <- err
			return
		}
		for index := range payload {
			payload[index] ^= mask[index%4]
		}
		decoded, err := decodeCBOR(payload)
		if err != nil {
			serverDone <- err
			return
		}
		root := decoded.(map[any]any)
		data := root["data"].(map[any]any)
		if _, err = server.Write([]byte{0x89, 1, 'p'}); err != nil {
			serverDone <- err
			return
		}
		pongHeader := make([]byte, 2)
		if _, err = io.ReadFull(reader, pongHeader); err != nil {
			serverDone <- err
			return
		}
		if pongHeader[0] != 0x8a || pongHeader[1]&0x80 == 0 || pongHeader[1]&0x7f != 1 {
			serverDone <- errors.New("client did not answer ping with a masked pong")
			return
		}
		pongMask := make([]byte, 4)
		if _, err = io.ReadFull(reader, pongMask); err != nil {
			serverDone <- err
			return
		}
		pongPayload := make([]byte, 1)
		if _, err = io.ReadFull(reader, pongPayload); err != nil {
			serverDone <- err
			return
		}
		pongPayload[0] ^= pongMask[0]
		if pongPayload[0] != 'p' {
			serverDone <- errors.New("pong payload mismatch")
			return
		}
		summary := cborMap(cborText("v"), cborUint(2), cborText("type"), cborText("summary"), cborText("source"), cborText("device"), cborText("boot_id"), cborText("c6-test"), cborText("seq"), cborUint(1), cborText("captured_ms"), cborUint(50), cborText("data"), cborMap())
		if err = writeTestServerFrame(server, 2, summary); err != nil {
			serverDone <- err
			return
		}
		reply := cborMap(cborText("v"), cborUint(2), cborText("type"), cborText("echo.response"), cborText("request_id"), cborText(root["request_id"].(string)), cborText("data"), cborMap(cborText("text"), cborText(data["text"].(string))))
		if err = writeTestServerFrame(server, 2, reply); err != nil {
			serverDone <- err
			return
		}
		serverDone <- nil
	}()
	outcomes, err := wsExchange(client, bufio.NewReader(client), requestPayload)
	if err != nil {
		t.Fatal(err)
	}
	if outcomes.summaries != 1 || outcomes.pings != 1 {
		t.Fatalf("WS outcomes=%+v", outcomes)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
}

func writeTestServerFrame(connection net.Conn, opcode byte, payload []byte) error {
	frame := []byte{0x80 | opcode}
	if len(payload) < 126 {
		frame = append(frame, byte(len(payload)))
	} else {
		frame = append(frame, 126, byte(len(payload)>>8), byte(len(payload)))
	}
	return writeFull(connection, append(frame, payload...))
}

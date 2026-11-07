package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestNormalProfileRejectsStressDisguisedAsNormal(t *testing.T) {
	for _, item := range []struct {
		profile  string
		clients  int
		interval time.Duration
	}{
		{"unknown", 1, time.Second}, {"rest", 0, time.Second},
		{"rest", 4, time.Second}, {"ui", 3, 250 * time.Millisecond},
	} {
		err := runNormalProfile(item.profile, "http://127.0.0.1:12345", time.Second,
			item.clients, item.interval, "127.0.0.1", filepath.Join(t.TempDir(), "invalid.jsonl"))
		if err == nil {
			t.Fatalf("invalid normal profile accepted: %+v", item)
		}
	}
}

func TestRestProfileUsesOnlyPredictableRestRequests(t *testing.T) {
	// {"boot_id":"test"}: all diagnostic reads use a valid bounded CBOR map.
	body := append([]byte{0xa1, 0x67}, []byte("boot_id")...)
	body = append(body, 0x64)
	body = append(body, []byte("test")...)
	unexpected := make(chan string, 16)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "GET" || (r.URL.Path != "/health" && r.URL.Path != "/api/v2/info" && r.URL.Path != "/api/v2/summary") {
			unexpected <- r.Method + " " + r.URL.Path
			w.WriteHeader(500)
			return
		}
		_, _ = w.Write(body)
	}))
	defer server.Close()
	output := filepath.Join(t.TempDir(), "rest.jsonl")
	if err := runNormalProfile("rest", server.URL, 500*time.Millisecond, 1, time.Second, "127.0.0.1", output); err != nil {
		t.Fatal(err)
	}
	select {
	case path := <-unexpected:
		t.Fatalf("unexpected REST traffic: %s", path)
	default:
	}
	data, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	var final record
	for _, line := range bytes.Split(bytes.TrimSpace(data), []byte{10}) {
		if err = json.Unmarshal(line, &final); err != nil {
			t.Fatal(err)
		}
	}
	if final.Type != "final" || final.Counters == nil || final.Counters.HTTP == 0 || final.Counters.Assets != 0 || final.Counters.WSOpened != 0 || final.Counters.MCPList != 0 || final.Counters.SSEOpened != 0 {
		t.Fatalf("bad REST-only counters: %+v", final)
	}
}

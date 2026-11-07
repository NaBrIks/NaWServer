package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

// Bounded protocol acceptance, separate from sustained REST and overload.
// One admitted guest WS/SSE and the reserved REST automation must survive
// repeated attempts to exceed that same guest's stream quota.
func runQosStreams(base, local, output, primaryKey string) error {
	if primaryKey == "" {
		return fmt.Errorf("qos-streams requires the configured primary key")
	}
	options, err := validateOptions(base, time.Minute, 2, time.Second, local, output, "/", "cached")
	if err != nil {
		return err
	}
	file, err := os.OpenFile(output, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer file.Close()
	client, err := newHTTPClient(options)
	if err != nil {
		return err
	}
	defer client.CloseIdleConnections()
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	identity := getBootID(ctx, client, base)
	if identity == "" {
		return fmt.Errorf("missing boot identity")
	}
	const guest = "qos-stream-guest"
	ws, wsReader, wsResponse, err := connectWithKey(options, "/ws", "", guest)
	if err != nil {
		return err
	}
	defer ws.Close()
	if wsResponse.StatusCode != 101 {
		return fmt.Errorf("first guest WS status %d", wsResponse.StatusCode)
	}
	sse, sseReader, sseResponse, err := connectWithKey(options, "/api/v2/events", "", guest)
	if err != nil {
		return err
	}
	defer sse.Close()
	if sseResponse.StatusCode != 200 {
		return fmt.Errorf("first guest SSE status %d", sseResponse.StatusCode)
	}
	report := struct {
		Profile                                            string    `json:"profile"`
		At                                                 time.Time `json:"at"`
		Boot                                               string    `json:"boot_id"`
		WSRefused, SSERefused, Pongs, PrimaryOK, SSEEvents int
		MaxRefusalMS                                       float64 `json:"max_refusal_ms"`
		Pass                                               bool    `json:"pass"`
	}{Profile: "qos-streams", At: time.Now().UTC(), Boot: identity}
	started := time.Now()
	cadence := time.NewTicker(350 * time.Millisecond)
	defer cadence.Stop()
	for cycle := 0; cycle < 10; cycle++ {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		for _, path := range []string{"/ws", "/api/v2/events"} {
			before := time.Now()
			connection, _, response, connectErr := connectWithKey(options, path, "", guest)
			if connectErr != nil {
				return connectErr
			}
			_, readErr := io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
			response.Body.Close()
			connection.Close()
			if readErr != nil || response.StatusCode != 503 || response.Header.Get("Retry-After") == "" {
				return fmt.Errorf("%s refusal status=%d error=%v retry=%q", path, response.StatusCode, readErr, response.Header.Get("Retry-After"))
			}
			elapsed := float64(time.Since(before).Microseconds()) / 1000
			if elapsed > report.MaxRefusalMS {
				report.MaxRefusalMS = elapsed
			}
			if path == "/ws" {
				report.WSRefused++
			} else {
				report.SSERefused++
			}
		}
		ws.SetDeadline(time.Now().Add(3 * time.Second))
		if err := writeClientWSFrame(ws, 9, []byte("qos")); err != nil {
			return err
		}
		pong := false
		for frameIndex := 0; frameIndex < 16; frameIndex++ {
			frame, readErr := readWSFrame(wsReader)
			if readErr != nil {
				return readErr
			}
			if frame.opcode == 8 {
				return fmt.Errorf("admitted WS was closed")
			}
			if frame.opcode == 10 && string(frame.payload) == "qos" {
				pong = true
				break
			}
		}
		if !pong {
			return fmt.Errorf("missing admitted WS pong")
		}
		report.Pongs++
		request, _ := http.NewRequestWithContext(ctx, http.MethodGet, base+"/health", nil)
		request.Header.Set("X-Client-Key", primaryKey)
		response, requestErr := client.Do(request)
		if requestErr != nil {
			return requestErr
		}
		io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
		response.Body.Close()
		if response.StatusCode != 200 {
			return fmt.Errorf("primary health status %d", response.StatusCode)
		}
		report.PrimaryOK++
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-cadence.C:
		}
	}
	sse.SetDeadline(time.Now().Add(3 * time.Second))
	for lines := 0; lines < 128; lines++ {
		line, readErr := sseReader.ReadString('\n')
		if readErr != nil {
			return readErr
		}
		if strings.HasPrefix(line, "data:") {
			report.SSEEvents++
			break
		}
	}
	if report.SSEEvents == 0 {
		return fmt.Errorf("admitted SSE made no progress")
	}
	report.Pass = report.MaxRefusalMS < 1000 && getBootID(ctx, client, base) == identity
	if err := json.NewEncoder(file).Encode(report); err != nil {
		return err
	}
	fmt.Printf("qos-streams pass=%v WS503=%d SSE503=%d pongs=%d primary=%d elapsed=%s\n", report.Pass, report.WSRefused, report.SSERefused, report.Pongs, report.PrimaryOK, time.Since(started).Round(time.Millisecond))
	if !report.Pass {
		return fmt.Errorf("QoS stream acceptance failed")
	}
	return nil
}

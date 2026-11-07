package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"time"
)

// Negative resource checks are deliberately separate from normal clients.
// These limits match the current fixture's public configuration, not library defaults.
func runLimitProfile(base, local, output string, headerLimit int) error {
	if headerLimit < 128 || headerLimit > 65536 {
		return fmt.Errorf("header limit must be 128..65536")
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
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	identity := getBootID(ctx, client, base)
	if identity == "" {
		return fmt.Errorf("boot identity unavailable")
	}
	type result struct {
		Check    string `json:"check"`
		Expected string `json:"expected"`
		Actual   string `json:"actual"`
		Pass     bool   `json:"pass"`
	}
	results := []result{}
	for _, item := range []struct {
		name     string
		bytes    int
		header   bool
		expected int
	}{
		// The current common dispatcher reports an oversized body as HTTP 400.
		// Resource checks follow that existing contract rather than inventing 413.
		{"body_4097_over_4096", 4097, false, 400},
		{fmt.Sprintf("headers_over_%d", headerLimit), headerLimit + 1, true, 431},
	} {
		var body io.Reader
		method := http.MethodGet
		if !item.header {
			body = bytes.NewReader(make([]byte, item.bytes))
			method = http.MethodPost
		}
		request, _ := http.NewRequestWithContext(ctx, method, base+"/health", body)
		if item.header {
			request.Header.Set("X-Limit-Probe", strings.Repeat("x", item.bytes))
		}
		response, requestErr := client.Do(request)
		actual := fmt.Sprint(requestErr)
		pass := false
		if requestErr == nil {
			_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
			response.Body.Close()
			actual = fmt.Sprint(response.StatusCode)
			pass = response.StatusCode == item.expected
		}
		results = append(results, result{item.name, fmt.Sprint(item.expected), actual, pass})
	}
	// Give the single transport owner bounded time to observe the closes.
	select {
	case <-ctx.Done():
	case <-time.After(500 * time.Millisecond):
	}
	connection, reader, response, connectErr := connect(options, "/ws", "")
	closeCode := 0
	detail := ""
	if connectErr != nil {
		detail = connectErr.Error()
	} else if response.StatusCode != 101 {
		detail = fmt.Sprintf("handshake HTTP %d", response.StatusCode)
	}
	if connectErr == nil && response.StatusCode == 101 {
		_ = connection.SetDeadline(time.Now().Add(4 * time.Second))
		if writeErr := writeClientWSFrame(connection, 2, make([]byte, 4097)); writeErr == nil {
			for index := 0; index < 8; index++ {
				frame, readErr := readWSFrame(reader)
				if readErr != nil {
					detail += " read=" + readErr.Error()
					break
				}
				detail += fmt.Sprintf(" frame=%d/%d", frame.opcode, len(frame.payload))
				if frame.opcode == 8 && len(frame.payload) >= 2 {
					closeCode = int(binary.BigEndian.Uint16(frame.payload))
					break
				}
			}
		}
	}
	if connection != nil {
		connection.Close()
	}
	results = append(results, result{"ws_message_4097_over_4096", "close 1009", fmt.Sprint(closeCode) + detail, closeCode == 1009})

	owned := []net.Conn{}
	defer func() {
		for _, connection := range owned {
			connection.Close()
		}
	}()
	refused := false
	for index := 0; index < 6; index++ {
		connection, _, response, connectErr := connect(options, "/ws", "")
		if connection != nil {
			owned = append(owned, connection)
		}
		if connectErr != nil {
			results = append(results, result{"ws_client_limit", "503 refusal", connectErr.Error(), false})
			break
		}
		if response.StatusCode != 101 {
			refused = response.StatusCode == 503
			results = append(results, result{"ws_client_limit", "503 refusal", fmt.Sprintf("accepted_owned=%d then HTTP %d", index, response.StatusCode), refused})
			break
		}
	}
	if !refused && len(results) == 2 {
		results = append(results, result{"ws_client_limit", "503 refusal", "no refusal within six owned connections", false})
	}
	for _, connection := range owned {
		connection.Close()
	}
	owned = nil
	probeContext, probeCancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer probeCancel()
	recovery := recoveryProbe(probeContext, client, base)
	pass := recovery.HealthStatus == 200 && recovery.InfoStatus == 200 && recovery.BootID == identity
	for _, item := range results {
		pass = pass && item.Pass
	}
	report := struct {
		Profile  string      `json:"profile"`
		At       time.Time   `json:"at"`
		Base     string      `json:"base"`
		Results  []result    `json:"results"`
		Recovery probeResult `json:"recovery"`
		Pass     bool        `json:"pass"`
	}{"limits-negative", time.Now().UTC(), base, results, recovery, pass}
	if err = json.NewEncoder(file).Encode(report); err != nil {
		return err
	}
	if !pass {
		return fmt.Errorf("resource limit checks failed; see report")
	}
	fmt.Println("LIMIT_PROFILE_PASS")
	return nil
}

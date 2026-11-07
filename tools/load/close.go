package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"time"
)

// Ten sequential owned connections, separate from REST and overload. Measure
// the actual Close frame and TCP EOF; a successful send() alone proves neither.
func runCloseProfile(base, local, output string) error {
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
	ctx, cancel := context.WithTimeout(context.Background(), 110*time.Second)
	defer cancel()
	identity := getBootID(ctx, client, base)
	if identity == "" {
		return fmt.Errorf("boot identity unavailable")
	}
	type result struct {
		At       time.Time `json:"at"`
		Round    int       `json:"round"`
		Expected int       `json:"expected"`
		Code     int       `json:"code"`
		FrameMS  float64   `json:"frame_ms"`
		EOFMS    float64   `json:"eof_ms"`
		Error    string    `json:"error,omitempty"`
		Pass     bool      `json:"pass"`
	}
	results := []result{}
	for round := 0; round < 5 && ctx.Err() == nil; round++ {
		for _, expected := range []int{1000, 1009} {
			item := result{At: time.Now().UTC(), Round: round, Expected: expected}
			connection, reader, response, connectErr := connect(options, "/ws", "")
			if connectErr != nil {
				item.Error = connectErr.Error()
			} else if response.StatusCode != 101 {
				item.Error = fmt.Sprintf("handshake HTTP %d", response.StatusCode)
			} else {
				started := time.Now()
				_ = connection.SetDeadline(started.Add(4 * time.Second))
				opcode, payload := byte(8), []byte{3, 0xe8}
				if expected == 1009 {
					opcode, payload = 2, make([]byte, 4097)
				}
				probeErr := writeClientWSFrame(connection, opcode, payload)
				for count := 0; probeErr == nil && count < 16; count++ {
					frame, frameErr := readWSFrame(reader)
					probeErr = frameErr
					if frameErr == nil && frame.opcode == 8 && len(frame.payload) >= 2 {
						item.Code = int(binary.BigEndian.Uint16(frame.payload))
						item.FrameMS = float64(time.Since(started).Microseconds()) / 1000
						if expected == 1009 {
							probeErr = writeClientWSFrame(connection, 8, frame.payload)
						}
						if probeErr == nil {
							_, eofErr := reader.ReadByte()
							item.EOFMS = float64(time.Since(started).Microseconds()) / 1000
							if eofErr != io.EOF {
								probeErr = fmt.Errorf("after Close expected TCP EOF, got %v", eofErr)
							}
						}
						break
					}
				}
				if probeErr != nil {
					item.Error = probeErr.Error()
				}
				item.Pass = item.Code == expected && probeErr == nil
			}
			if connection != nil {
				connection.Close()
			}
			results = append(results, item)
		}
	}
	probeContext, probeCancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer probeCancel()
	recovery := recoveryProbe(probeContext, client, base)
	pass := len(results) == 10 && recovery.BootID == identity && recovery.HealthStatus == 200 && recovery.InfoStatus == 200
	for _, item := range results {
		pass = pass && item.Pass
	}
	report := struct {
		Profile  string      `json:"profile"`
		Base     string      `json:"base"`
		BootID   string      `json:"boot_id"`
		Results  []result    `json:"results"`
		Recovery probeResult `json:"recovery"`
		Pass     bool        `json:"pass"`
	}{"ws-close", base, identity, results, recovery, pass}
	if err := json.NewEncoder(file).Encode(report); err != nil {
		return err
	}
	if !pass {
		return fmt.Errorf("Close frame/TCP EOF acceptance failed; see report")
	}
	fmt.Println("WS_CLOSE_PROFILE_PASS 10/10")
	return nil
}

package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
)

// Minimal's protected binary acceptance endpoint; no payload codec is involved.
func checkBodyAPI(client *http.Client, base string) error {
	for _, mode := range []string{"raw", "pointer", "c-array", "array", "vector"} {
		sizes := []int{0, 1, 7, 64}
		if mode == "raw" || mode == "vector" {
			sizes = append(sizes, 65, 4096)
		} else {
			sizes = append(sizes, 65) // Must fail without truncating.
		}
		for _, size := range sizes {
			payload := make([]byte, size)
			for i := range payload {
				payload[i] = byte(i * 37)
			}
			req, err := http.NewRequest(http.MethodPost, base+"/private/body", bytes.NewReader(payload))
			if err != nil {
				return err
			}
			req.SetBasicAuth("test", "test-only") // Public fixture only.
			req.Header.Set("Content-Type", "application/octet-stream")
			req.Header.Set("X-Body-Copy", mode)
			response, err := client.Do(req)
			if err != nil {
				return fmt.Errorf("body %s/%d: %w", mode, size, err)
			}
			actual, err := io.ReadAll(io.LimitReader(response.Body, 8193))
			response.Body.Close()
			if err != nil {
				return err
			}
			wantStatus := http.StatusOK
			if size > 64 && mode != "raw" && mode != "vector" {
				wantStatus = http.StatusRequestEntityTooLarge
			}
			if response.StatusCode != wantStatus {
				return fmt.Errorf("body %s/%d: status %d, expected %d", mode, size, response.StatusCode, wantStatus)
			}
			if wantStatus == http.StatusOK && (!bytes.Equal(actual, payload) || response.Header.Get("Content-Type") != "application/octet-stream") {
				return fmt.Errorf("body %s/%d: byte/MIME mismatch", mode, size)
			}
		}
	}
	if err := expectHTTP(client, base, "/private/body", "POST", "", "", 401, nil); err != nil {
		return err
	}
	fmt.Println("PASS raw body and pointer/C-array/std::array/vector copies, NUL/binary 0..4096, overflow and auth")
	return nil
}

package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"sort"
	"sync"
	"time"
)

type qosClass struct {
	OK        int            `json:"ok"`
	Busy      int            `json:"http_503"`
	Rate      int            `json:"http_429"`
	Errors    int            `json:"errors"`
	Timeouts  int            `json:"timeouts"`
	MaxMS     float64        `json:"max_ms"`
	P95MS     float64        `json:"p95_ms"`
	MinRAM    uint64         `json:"min_sampled_internal_free"`
	Reasons   map[string]int `json:"error_reasons,omitempty"`
	latencies []float64
}

// One protected automation, two mixed rate/capacity workers and an anonymous
// keeper. Four outstanding connections maximum; no SDK/browser/PC changes.
func runQosProfile(base, local, output, primaryKey string, duration time.Duration) error {
	if primaryKey == "" || duration < time.Second || duration > 100*time.Second {
		return fmt.Errorf("qos requires a primary key and duration 1..100s (plus bounded cleanup)")
	}
	options, err := validateOptions(base, duration, 2, time.Second, local, output, "/", "cached")
	if err != nil {
		return err
	}
	file, err := os.OpenFile(output, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer file.Close()
	primary, err := newHTTPClient(options)
	if err != nil {
		return err
	}
	defer primary.CloseIdleConnections()
	shared, err := newHTTPClient(options)
	if err != nil {
		return err
	}
	defer shared.CloseIdleConnections()
	perform := func(ctx context.Context, client *http.Client, path, key string) (int, []byte, error) {
		request, requestErr := http.NewRequestWithContext(ctx, "GET", base+path, nil)
		if requestErr != nil {
			return 0, nil, requestErr
		}
		if key != "" {
			request.Header.Set("X-Client-Key", key)
		}
		response, requestErr := client.Do(request)
		if requestErr != nil {
			return 0, nil, requestErr
		}
		defer response.Body.Close()
		body, requestErr := io.ReadAll(io.LimitReader(response.Body, maximumHTTPBody+1))
		if requestErr == nil && len(body) > maximumHTTPBody {
			requestErr = fmt.Errorf("body exceeded cap")
		}
		if (response.StatusCode == 429 || response.StatusCode == 503) && response.Header.Get("Retry-After") == "" {
			requestErr = fmt.Errorf("denial omitted Retry-After")
		}
		return response.StatusCode, body, requestErr
	}
	warm, warmCancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer warmCancel()
	status, identityBody, err := perform(warm, primary, "/api/v2/info", primaryKey)
	identity := cborBootID(identityBody)
	if err != nil || status != 200 || identity == "" {
		return fmt.Errorf("primary identity unavailable: status=%d error=%v", status, err)
	}
	const guest = "qos-guest-held"
	for _, key := range []string{"", guest} {
		status, _, err = perform(warm, shared, "/health", key)
		if err != nil || status != 200 {
			return fmt.Errorf("guest reservation warmup failed: status=%d error=%v", status, err)
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), duration)
	defer cancel()
	started := time.Now()
	var mutex sync.Mutex
	classes := map[string]*qosClass{}
	for _, name := range []string{"primary", "anonymous_keeper", "guest_rate", "extra_capacity"} {
		classes[name] = &qosClass{Reasons: map[string]int{}}
	}
	var workers sync.WaitGroup
	worker := func(interval time.Duration, selectRequest func(int) (string, string, string), client *http.Client) {
		workers.Add(1)
		go func() {
			defer workers.Done()
			ticker := time.NewTicker(interval)
			defer ticker.Stop()
			for cycle := 0; ctx.Err() == nil; cycle++ {
				name, path, key := selectRequest(cycle)
				requestStarted := time.Now()
				code, body, requestErr := perform(ctx, client, path, key)
				elapsed := float64(time.Since(requestStarted).Microseconds()) / 1000
				if ctx.Err() != nil {
					return
				}
				mutex.Lock()
				value := classes[name]
				value.latencies = append(value.latencies, elapsed)
				if elapsed > value.MaxMS {
					value.MaxMS = elapsed
				}
				if requestErr == nil {
					switch code {
					case 200:
						value.OK++
					case 503:
						value.Busy++
					case 429:
						value.Rate++
					default:
						requestErr = fmt.Errorf("unexpected HTTP %d", code)
					}
				}
				if requestErr == nil && name == "primary" && code == 200 {
					if cborBootID(body) != identity {
						requestErr = fmt.Errorf("invalid summary or boot identity changed")
					}
					if decoded, decodeErr := decodeCBOR(body); decodeErr == nil {
						if root, ok := decoded.(map[any]any); ok {
							if data, ok := root["data"].(map[any]any); ok {
								if regions, ok := data["memory"].([]any); ok {
									for _, item := range regions {
										if region, ok := item.(map[any]any); ok && region["id"] == "internal" {
											if free, ok := region["free_bytes"].(uint64); ok && (value.MinRAM == 0 || free < value.MinRAM) {
												value.MinRAM = free
											}
										}
									}
								}
							}
						}
					}
				}
				if requestErr != nil {
					value.Errors++
					var networkError net.Error
					if errors.As(requestErr, &networkError) && networkError.Timeout() {
						value.Timeouts++
					}
					if len(value.Reasons) < 16 {
						value.Reasons[requestErr.Error()]++
					}
				}
				mutex.Unlock()
				select {
				case <-ctx.Done():
					return
				case <-ticker.C:
				}
			}
		}()
	}
	worker(500*time.Millisecond, func(int) (string, string, string) { return "primary", "/api/v2/summary", primaryKey }, primary)
	worker(time.Second, func(int) (string, string, string) { return "anonymous_keeper", "/health", "" }, shared)
	for index := 0; index < 2; index++ {
		index := index
		worker(50*time.Millisecond, func(cycle int) (string, string, string) {
			if cycle%4 == 3 {
				return "extra_capacity", "/health", fmt.Sprintf("qos-extra-%d-%d", index, cycle)
			}
			return "guest_rate", "/health", guest
		}, shared)
	}
	workers.Wait()
	probe, probeCancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer probeCancel()
	health, _, healthErr := perform(probe, primary, "/health", primaryKey)
	info, finalIdentity, infoErr := perform(probe, primary, "/api/v2/info", primaryKey)
	pass := healthErr == nil && infoErr == nil && health == 200 && info == 200 && cborBootID(finalIdentity) == identity
	for name, value := range classes {
		if len(value.latencies) > 0 {
			sort.Float64s(value.latencies)
			value.P95MS = value.latencies[(len(value.latencies)-1)*95/100]
		}
		pass = pass && value.Errors == 0
		if name == "primary" || name == "anonymous_keeper" {
			pass = pass && value.OK > 0 && value.Busy == 0 && value.Rate == 0
		}
	}
	pass = pass && classes["extra_capacity"].Busy > 0 && classes["extra_capacity"].OK == 0 &&
		classes["extra_capacity"].Rate == 0 && classes["extra_capacity"].MaxMS < 1000 && classes["guest_rate"].Rate > 0
	report := struct {
		Profile string               `json:"profile"`
		At      time.Time            `json:"at"`
		Base    string               `json:"base"`
		Boot    string               `json:"boot_id"`
		Elapsed float64              `json:"elapsed_seconds"`
		Classes map[string]*qosClass `json:"classes"`
		Pass    bool                 `json:"pass"`
	}{"qos-primary-capacity-rate", time.Now().UTC(), base, identity, time.Since(started).Seconds(), classes, pass}
	if err := json.NewEncoder(file).Encode(report); err != nil {
		return err
	}
	fmt.Printf("qos pass=%v primary=%d busy=%d rate=%d\n", pass, classes["primary"].OK, classes["extra_capacity"].Busy, classes["guest_rate"].Rate)
	if !pass {
		return fmt.Errorf("QoS acceptance failed; see report")
	}
	return nil
}

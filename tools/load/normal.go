package main

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"
)

// Separate acceptance profiles: no mixed stress, invalid commands or hidden WS
// clients in a REST automation run. Each worker has one outstanding HTTP request.
func runNormalProfile(profile, base string, duration time.Duration, clients int, interval time.Duration, local, output string) error {
	if profile != "rest" && profile != "ui" {
		return fmt.Errorf("profile must be rest, ui or stress")
	}
	if clients < 1 || clients > 3 {
		return fmt.Errorf("normal profiles require 1, 2 or 3 clients")
	}
	if interval < time.Second {
		return fmt.Errorf("normal profile interval must be at least 1s")
	}
	options, err := validateOptions(base, duration, 2, interval, local, output, "/", "cached")
	if err != nil {
		return err
	}
	options.clients = clients
	options.startedAt = time.Now()
	if output == "" {
		return fmt.Errorf("normal profile requires an explicit output path")
	}
	file, err := os.OpenFile(output, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer file.Close()
	writer := &outputWriter{file: file}
	runID := fmt.Sprintf("%s-%d", options.startedAt.UTC().Format("20060102T150405Z"), os.Getpid())
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	ctx, cancel := context.WithTimeout(ctx, duration)
	defer cancel()
	stats := newCounters(clients)
	client, err := newHTTPClient(options)
	if err != nil {
		return err
	}
	defer client.CloseIdleConnections()
	identity := getBootID(ctx, client, base)
	if identity == "" {
		return fmt.Errorf("device boot identity unavailable")
	}
	stats.setBootID(identity)
	if err = writer.write(record{Type: "stage", At: time.Now().UTC(), RunID: runID, Stage: profile + "-normal-start", Base: base, Clients: clients, HTTPConnectionLimit: clients, Interval: interval.String(), Message: "one bounded sequential HTTP worker per client; REST has no assets/MCP/WS/SSE; UI assets once per client, then status/info"}); err != nil {
		return err
	}
	var workers sync.WaitGroup
	for index := 0; index < clients; index++ {
		workers.Add(1)
		go func(index int) {
			defer workers.Done()
			// Stagger automation polling; a burst is tested as its own profile.
			timer := time.NewTimer(time.Duration(index) * interval / time.Duration(clients))
			defer timer.Stop()
			select {
			case <-ctx.Done():
				return
			case <-timer.C:
			}
			if profile == "ui" {
				paths, discoverErr := discoverAssetPaths(ctx, client, base, "/")
				if discoverErr == nil {
					discoverErr = prefetchStaticAssets(ctx, client, base, paths, stats)
				}
				if discoverErr != nil && ctx.Err() == nil {
					stats.failure(index, discoverErr)
				}
			}
			ticker := time.NewTicker(interval)
			defer ticker.Stop()
			for cycle := 0; ; cycle++ {
				var requestErr error
				if cycle%10 == 9 {
					requestErr = fetchBody(ctx, client, base+"/api/v2/info", index, stats, "info", func(status int, body []byte) error {
						if status != http.StatusOK {
							return fmt.Errorf("info status=%d", status)
						}
						boot := cborBootID(body)
						if boot != identity {
							cancel()
							return fmt.Errorf("boot_id changed")
						}
						return nil
					})
				} else {
					requestErr = fetchSummary(ctx, client, base, index, stats)
				}
				if requestErr != nil && ctx.Err() == nil {
					stats.failure(index, requestErr)
				}
				select {
				case <-ctx.Done():
					return
				case <-ticker.C:
				}
			}
		}(index)
	}
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	var writeErr error
loop:
	for {
		select {
		case <-ctx.Done():
			break loop
		case now := <-ticker.C:
			snapshot := stats.snapshot()
			writeErr = writer.write(record{Type: "sample", At: now.UTC(), ElapsedMS: time.Since(options.startedAt).Milliseconds(), RunID: runID, Stage: profile + "-normal", Base: base, Clients: clients, HTTPConnectionLimit: clients, Interval: interval.String(), Counters: &snapshot})
			if writeErr != nil {
				cancel()
				break loop
			}
		}
	}
	workers.Wait()
	client.CloseIdleConnections()
	snapshot := stats.snapshot()
	recoveryContext, recoveryCancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer recoveryCancel()
	recovery := recoveryProbe(recoveryContext, client, base)
	if writeErr != nil {
		return writeErr
	}
	if err = writer.write(record{Type: "final", At: time.Now().UTC(), ElapsedMS: time.Since(options.startedAt).Milliseconds(), RunID: runID, Stage: profile + "-normal-final", Base: base, Clients: clients, HTTPConnectionLimit: clients, Interval: interval.String(), Counters: &snapshot, Recovery: &recovery}); err != nil {
		return err
	}
	fmt.Printf("profile=%s clients=%d http=%d errors=%d timeouts=%d recovery=%d/%d\n", profile, clients, snapshot.HTTP, snapshot.Errors, snapshot.Timeouts, recovery.HealthStatus, recovery.InfoStatus)
	if snapshot.Errors != 0 || recovery.HealthStatus != 200 || recovery.InfoStatus != 200 || recovery.BootID != identity {
		return fmt.Errorf("normal acceptance failed")
	}
	return nil
}

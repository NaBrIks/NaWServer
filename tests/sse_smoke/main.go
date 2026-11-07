package main

import (
	"bufio"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"mime"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

const timeout = 15 * time.Second

var checks int

type result struct {
	BaseURL string   `json:"base_url"`
	Checks  int      `json:"checks"`
	Errors  []string `json:"errors"`
	Stats   stats    `json:"stats"`
}

type stats struct {
	Active      int `json:"active"`
	Begun       int `json:"begun"`
	Completed   int `json:"completed"`
	QueuedBytes int `json:"queued_bytes"`
	RolledBack  int `json:"rolled_back"`
}

type event struct {
	Name  string
	Data  string
	ID    string
	Retry int
}

type subscription struct {
	response *http.Response
	cancel   context.CancelFunc
	reader   *bufio.Reader
	lastID   string
	closed   bool
}

type cancelBody struct {
	io.ReadCloser
	cancel context.CancelFunc
}

func (body *cancelBody) Close() error {
	err := body.ReadCloser.Close()
	body.cancel()
	return err
}

func expect(ok bool, format string, args ...any) error {
	checks++
	if !ok {
		return fmt.Errorf(format, args...)
	}
	return nil
}

func newClient() *http.Client {
	transport := &http.Transport{ResponseHeaderTimeout: 5 * time.Second}
	return &http.Client{Transport: transport}
}

func request(client *http.Client, base, method, path, body string, auth bool) (*http.Response, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	req, err := http.NewRequestWithContext(ctx, method, base+path, strings.NewReader(body))
	if err != nil {
		cancel()
		return nil, err
	}
	if auth {
		req.SetBasicAuth("test", "test-only")
	}
	response, err := client.Do(req)
	if err != nil {
		cancel()
		return nil, err
	}
	response.Body = &cancelBody{ReadCloser: response.Body, cancel: cancel}
	return response, nil
}

func open(client *http.Client, base, path string, lastID *string, auth bool) (*subscription, *http.Response, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+path, nil)
	if err != nil {
		cancel()
		return nil, nil, err
	}
	if lastID != nil {
		req.Header.Set("Last-Event-ID", *lastID)
	}
	if auth {
		req.SetBasicAuth("test", "test-only")
	}
	response, err := client.Do(req)
	if err != nil {
		cancel()
		return nil, nil, err
	}
	return &subscription{
		response: response,
		cancel:   cancel,
		reader:   bufio.NewReader(response.Body),
	}, response, nil
}

func (s *subscription) next() (event, error) {
	for {
		item := event{Name: "message", ID: s.lastID}
		var data []string
		seen := false
		for {
			line, err := s.reader.ReadString('\n')
			if err != nil {
				return event{}, err
			}
			line = strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r")
			if line == "" {
				if !seen {
					break
				}
				item.Data = strings.Join(data, "\n")
				s.lastID = item.ID
				return item, nil
			}
			if strings.HasPrefix(line, ":") {
				continue
			}
			field, value, found := strings.Cut(line, ":")
			if found {
				value = strings.TrimPrefix(value, " ")
			}
			switch field {
			case "data":
				seen = true
				data = append(data, value)
			case "event":
				if value != "" {
					item.Name = value
				}
			case "id":
				item.ID = value
			case "retry":
				fmt.Sscan(value, &item.Retry)
			}
		}
	}
}

func (s *subscription) close() {
	if s.closed {
		return
	}
	s.closed = true
	s.cancel()
	_ = s.response.Body.Close()
}

func checkConnected(s *subscription, want string) error {
	item, err := s.next()
	if err != nil {
		return err
	}
	return expect(item.Name == "connected" && item.Data == want && item.ID == "boot:1" && item.Retry == 1000,
		"unexpected initial SSE event: %#v", item)
}

func getStats(client *http.Client, base string) (stats, error) {
	response, err := request(client, base, http.MethodGet, "/events/stats", "", false)
	if err != nil {
		return stats{}, err
	}
	defer response.Body.Close()
	var value stats
	if response.StatusCode != http.StatusOK {
		return value, fmt.Errorf("stats status %d", response.StatusCode)
	}
	err = json.NewDecoder(io.LimitReader(response.Body, 4096)).Decode(&value)
	return value, err
}

func closeAll(client *http.Client, base string, subscribers []*subscription) error {
	response, err := request(client, base, http.MethodPost, "/events/close", "", false)
	if err != nil {
		return err
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 1024))
	response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("close status %d", response.StatusCode)
	}
	for _, subscriber := range subscribers {
		if subscriber.closed {
			continue
		}
		_, err = io.Copy(io.Discard, io.LimitReader(subscriber.reader, 8192))
		if err != nil {
			return fmt.Errorf("SSE did not close cleanly: %w", err)
		}
		subscriber.close()
	}
	return nil
}

func run(base string) (stats, error) {
	client := newClient()
	defer client.CloseIdleConnections()
	var openStreams []*subscription
	defer func() {
		for _, subscriber := range openStreams {
			subscriber.close()
		}
	}()
	defer func() {
		if len(openStreams) > 0 {
			_ = closeAll(client, base, openStreams)
		}
	}()

	response, err := request(client, base, http.MethodGet, "/health", "", false)
	if err != nil {
		return stats{}, err
	}
	health, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
	response.Body.Close()
	var healthState struct {
		OK   bool   `json:"ok"`
		HTTP string `json:"http"`
	}
	decodeErr := json.Unmarshal(health, &healthState)
	if err = expect(response.StatusCode == 200 && decodeErr == nil && (healthState.OK || healthState.HTTP == "ok"),
		"health status/body: %d %s", response.StatusCode, health); err != nil {
		return stats{}, err
	}

	response, err = request(client, base, http.MethodGet, "/events/private", "", false)
	if err != nil {
		return stats{}, err
	}
	io.Copy(io.Discard, io.LimitReader(response.Body, 1024))
	response.Body.Close()
	if err = expect(response.StatusCode == http.StatusUnauthorized, "private endpoint status %d", response.StatusCode); err != nil {
		return stats{}, err
	}
	private, response, err := open(client, base, "/events/private", nil, true)
	if err != nil {
		return stats{}, err
	}
	if err = expect(response.StatusCode == http.StatusOK, "authenticated private status %d", response.StatusCode); err != nil {
		response.Body.Close()
		return stats{}, err
	}
	if err = checkHeaders(response); err != nil {
		response.Body.Close()
		return stats{}, err
	}
	openStreams = append(openStreams, private)
	if err = checkConnected(private, "ready"); err != nil {
		return stats{}, err
	}
	if err = closeAll(client, base, []*subscription{private}); err != nil {
		return stats{}, err
	}

	for _, tc := range []struct {
		id   *string
		want string
	}{{nil, "ready"}, {ptr(""), ""}, {ptr(strings.Repeat("i", 128)), strings.Repeat("i", 128)}} {
		subscriber, response, openErr := open(client, base, "/events", tc.id, true)
		if openErr != nil {
			return stats{}, openErr
		}
		if err = expect(response.StatusCode == http.StatusOK, "Last-Event-ID probe status %d", response.StatusCode); err != nil {
			response.Body.Close()
			return stats{}, err
		}
		if err = checkHeaders(response); err != nil {
			response.Body.Close()
			return stats{}, err
		}
		openStreams = append(openStreams, subscriber)
		if err = checkConnected(subscriber, tc.want); err != nil {
			return stats{}, err
		}
		if err = closeAll(client, base, []*subscription{subscriber}); err != nil {
			return stats{}, err
		}
	}
	tooLong, response, err := open(client, base, "/events", ptr(strings.Repeat("i", 129)), true)
	if err != nil {
		return stats{}, err
	}
	io.Copy(io.Discard, io.LimitReader(response.Body, 1024))
	tooLong.close()
	if err = expect(response.StatusCode == http.StatusBadRequest,
		"129-byte Last-Event-ID status %d", response.StatusCode); err != nil {
		return stats{}, err
	}

	bodyRequest, err := request(client, base, http.MethodGet, "/events", "body", true)
	if err != nil {
		return stats{}, err
	}
	io.Copy(io.Discard, io.LimitReader(bodyRequest.Body, 1024))
	bodyRequest.Body.Close()
	if err = expect(bodyRequest.StatusCode == http.StatusBadRequest, "GET with body status %d", bodyRequest.StatusCode); err != nil {
		return stats{}, err
	}

	for index := 0; index < 4; index++ {
		subscriber, response, openErr := open(client, base, "/events", nil, true)
		if openErr != nil {
			return stats{}, openErr
		}
		if err = expect(response.StatusCode == http.StatusOK, "subscriber %d status %d", index+1, response.StatusCode); err != nil {
			response.Body.Close()
			return stats{}, err
		}
		if err = checkHeaders(response); err != nil {
			response.Body.Close()
			return stats{}, err
		}
		openStreams = append(openStreams, subscriber)
		if err = checkConnected(subscriber, "ready"); err != nil {
			return stats{}, err
		}
	}
	fifth, response, err := open(client, base, "/events", nil, true)
	if err != nil {
		return stats{}, err
	}
	io.Copy(io.Discard, io.LimitReader(response.Body, 1024))
	fifth.close()
	if err = expect(response.StatusCode == http.StatusServiceUnavailable,
		"fifth subscriber status %d", response.StatusCode); err != nil {
		return stats{}, err
	}

	healthResponse, err := request(client, base, http.MethodGet, "/health", "", false)
	if err != nil {
		return stats{}, err
	}
	io.Copy(io.Discard, io.LimitReader(healthResponse.Body, 4096))
	healthResponse.Body.Close()
	if err = expect(healthResponse.StatusCode == http.StatusOK, "health with 4 SSE status %d", healthResponse.StatusCode); err != nil {
		return stats{}, err
	}

	data := "Привет, 世界 🙂\nline two\n\ntrailing\n\x00tail\n"
	publish, err := request(client, base, http.MethodPost, "/events/publish?id=", data, false)
	if err != nil {
		return stats{}, err
	}
	publishBody, publishReadErr := io.ReadAll(io.LimitReader(publish.Body, 1024))
	publish.Body.Close()
	if err = expect(publish.StatusCode == http.StatusOK && string(publishBody) == "queued",
		"publish status/body %d %q length=%d read_error=%v headers=%v", publish.StatusCode,
		publishBody, publish.ContentLength, publishReadErr, publish.Header); err != nil {
		return stats{}, err
	}
	for _, subscriber := range openStreams[len(openStreams)-4:] {
		item, readErr := subscriber.next()
		if readErr != nil {
			return stats{}, readErr
		}
		if err = expect(item.Name == "reading" && item.Data == data && item.ID == "" && item.Retry == 1000,
			"broadcast event mismatch: %#v", item); err != nil {
			return stats{}, err
		}
	}

	finalStats, err := getStats(client, base)
	if err != nil {
		return stats{}, err
	}
	if err = expect(finalStats.Active == 4 && finalStats.QueuedBytes == 0,
		"stats during streams: %+v", finalStats); err != nil {
		return finalStats, err
	}
	if err = closeAll(client, base, openStreams[len(openStreams)-4:]); err != nil {
		return stats{}, err
	}
	finalStats, err = waitClosed(client, base)
	if err != nil {
		return finalStats, err
	}
	if err = expect(finalStats.Active == 0 && finalStats.Begun == finalStats.Completed &&
		finalStats.QueuedBytes == 0, "closed stats: %+v", finalStats); err != nil {
		return finalStats, err
	}

	// Reuse all slots after close; the server must not keep closed token generations live.
	reconnected, response, err := open(client, base, "/events", nil, true)
	if err != nil {
		return finalStats, err
	}
	if err = expect(response.StatusCode == http.StatusOK, "reconnect status %d", response.StatusCode); err != nil {
		response.Body.Close()
		return finalStats, err
	}
	openStreams = append(openStreams, reconnected)
	if err = checkConnected(reconnected, "ready"); err != nil {
		return finalStats, err
	}
	if err = closeAll(client, base, []*subscription{reconnected}); err != nil {
		return finalStats, err
	}
	finalStats, err = waitClosed(client, base)
	if err != nil {
		return finalStats, err
	}
	if err = expect(finalStats.Active == 0 && finalStats.Begun == finalStats.Completed &&
		finalStats.QueuedBytes == 0, "reconnect closed stats: %+v", finalStats); err != nil {
		return finalStats, err
	}
	return finalStats, nil
}

func runStop(base string) (stats, error) {
	client := newClient()
	defer client.CloseIdleConnections()
	var subscribers []*subscription
	defer func() {
		for _, subscriber := range subscribers {
			subscriber.close()
		}
	}()
	for index := 0; index < 4; index++ {
		subscriber, response, err := open(client, base, "/events", nil, true)
		if err != nil {
			return stats{}, err
		}
		if err = expect(response.StatusCode == http.StatusOK, "stop-mode subscriber %d status %d", index+1, response.StatusCode); err != nil {
			subscriber.close()
			return stats{}, err
		}
		if err = checkHeaders(response); err != nil {
			subscriber.close()
			return stats{}, err
		}
		subscribers = append(subscribers, subscriber)
		if err = checkConnected(subscriber, "ready"); err != nil {
			return stats{}, err
		}
	}
	response, err := request(client, base, http.MethodPost, "/events/stop", "", false)
	if err != nil {
		return stats{}, err
	}
	body, readErr := io.ReadAll(io.LimitReader(response.Body, 1024))
	response.Body.Close()
	if err = expect(response.StatusCode == http.StatusAccepted && readErr == nil && string(body) == "stop requested",
		"stop request status/body %d %q: %v", response.StatusCode, body, readErr); err != nil {
		return stats{}, err
	}
	for _, subscriber := range subscribers {
		_, streamErr := io.Copy(io.Discard, io.LimitReader(subscriber.reader, 8192))
		if err = expect(streamErr == nil, "stop did not end SSE stream cleanly: %v", streamErr); err != nil {
			return stats{}, err
		}
		subscriber.close()
	}
	value, err := waitClosed(client, base)
	if err != nil {
		return value, err
	}
	if err = expect(value.Active == 0 && value.Begun == value.Completed && value.QueuedBytes == 0,
		"stop-mode final stats: %+v", value); err != nil {
		return value, err
	}
	return value, nil
}

func checkHeaders(response *http.Response) error {
	mediaType, _, err := mime.ParseMediaType(response.Header.Get("Content-Type"))
	if err != nil {
		return err
	}
	if err = expect(mediaType == "text/event-stream", "SSE MIME %q", mediaType); err != nil {
		return err
	}
	return expect(response.Header.Get("X-Sse-Test") == "shared", "X-Sse-Test header %q", response.Header.Get("X-Sse-Test"))
}

func waitClosed(client *http.Client, base string) (stats, error) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		value, err := getStats(client, base)
		if err != nil {
			return stats{}, err
		}
		if value.Active == 0 {
			return value, nil
		}
		time.Sleep(20 * time.Millisecond)
	}
	return stats{}, fmt.Errorf("SSE sessions did not close within %s", timeout)
}

func ptr(value string) *string { return &value }

func main() {
	base := flag.String("url", "http://127.0.0.1:8080", "base URL of the SSE fixture")
	stopMode := flag.Bool("stop", false, "open four SSE requests and validate owner-driven stop")
	flag.Parse()
	baseURL, err := url.Parse(strings.TrimRight(*base, "/"))
	errors := []string{}
	var finalStats stats
	if err != nil || baseURL.Scheme == "" || baseURL.Host == "" {
		errors = append(errors, "invalid base URL")
	} else {
		if *stopMode {
			finalStats, err = runStop(strings.TrimRight(*base, "/"))
		} else {
			finalStats, err = run(strings.TrimRight(*base, "/"))
		}
		if err != nil {
			errors = append(errors, err.Error())
		}
	}
	output := result{BaseURL: strings.TrimRight(*base, "/"), Checks: checks, Errors: errors, Stats: finalStats}
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	_ = encoder.Encode(output)
	if len(errors) != 0 {
		os.Exit(1)
	}
}

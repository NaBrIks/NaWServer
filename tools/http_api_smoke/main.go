// Checks the new synchronous HTTP API on examples/minimal. No device mutation.
package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

var checks int

func main() {
	base, maximumClients, err := parseArguments(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		fmt.Fprintln(os.Stderr, "usage: http_api_smoke [--max-concurrent-clients=2|4] http://DEVICE")
		os.Exit(2)
	}
	if err := run(base, maximumClients); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("PASS: %d HTTP response checks, including %s concurrent clients\n", checks, profileLabel(maximumClients))
}

func parseArguments(arguments []string) (string, int, error) {
	flags := flag.NewFlagSet("http_api_smoke", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	maximumClients := flags.Int("max-concurrent-clients", 4, "maximum concurrent client profile: 2 or 4")
	if err := flags.Parse(arguments); err != nil {
		return "", 0, err
	}
	if flags.NArg() != 1 {
		return "", 0, fmt.Errorf("expected exactly one device URL")
	}
	if *maximumClients != 2 && *maximumClients != 4 {
		return "", 0, fmt.Errorf("--max-concurrent-clients must be 2 or 4")
	}
	return strings.TrimRight(flags.Arg(0), "/"), *maximumClients, nil
}

func concurrentProfiles(maximumClients int) []int {
	if maximumClients == 2 {
		return []int{2}
	}
	return []int{2, 4}
}

func profileLabel(maximumClients int) string {
	if maximumClients == 2 {
		return "2"
	}
	return "2/4"
}

func run(base string, maximumClients int) error {
	transport := &http.Transport{DisableCompression: true, MaxIdleConnsPerHost: 8}
	defer transport.CloseIdleConnections()
	client := &http.Client{Timeout: 8 * time.Second, Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	check := func(path string, want int, body []byte, headers map[string]string, requestHeaders map[string]string) error {
		req, _ := http.NewRequest("GET", base+path, nil)
		for k, v := range requestHeaders {
			req.Header.Set(k, v)
		}
		response, err := client.Do(req)
		if err != nil {
			return fmt.Errorf("%s: %w", path, err)
		}
		data, err := io.ReadAll(io.LimitReader(response.Body, 16385))
		response.Body.Close()
		if err != nil {
			return err
		}
		if response.StatusCode != want {
			return fmt.Errorf("%s status=%d expected=%d", path, response.StatusCode, want)
		}
		if body != nil && !bytes.Equal(body, data) {
			return fmt.Errorf("%s response bytes differ", path)
		}
		for k, v := range headers {
			if response.Header.Get(k) != v {
				return fmt.Errorf("%s header %s=%q expected %q", path, k, response.Header.Get(k), v)
			}
		}
		checks++
		return nil
	}
	cases := []struct {
		path                    string
		status                  int
		body                    []byte
		headers, requestHeaders map[string]string
	}{
		{"/health", 200, []byte(`{"ok":true}`), map[string]string{"Content-Type": "application/json", "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff", "X-Common": "server", "X-Response-API": "sync-v1"}, nil},
		{"/echo?value=a%00b%FF", 200, []byte{'a', 0, 'b', 255}, map[string]string{"Content-Type": "application/octet-stream"}, nil},
		{"/response/empty", 204, []byte{}, nil, nil},
		{"/response/redirect", 302, []byte{}, map[string]string{"Location": "/health"}, nil},
		{"/response/buffer?value=hello%20world", 200, []byte("value=hello world; n=42"), nil, nil},
		{"/response/headers", 200, []byte(`{"headers":true}`), map[string]string{"X-Common": "endpoint", "Cache-Control": "private", "Content-Type": "application/json", "X-Content-Type-Options": "nosniff"}, nil},
		{"/response/read-header", 200, []byte("from-client"), nil, map[string]string{"X-Test": "from-client"}},
		{"/response/read-header", 400, nil, nil, nil},
		{"/response/read-header", 400, nil, nil, map[string]string{"X-Test": strings.Repeat("a", 32)}},
		{"/response/private", 401, nil, map[string]string{"WWW-Authenticate": `Basic realm="nawserver"`}, nil},
		{"/response/private", 200, []byte("authorized"), nil, map[string]string{"Authorization": "Basic dGVzdDp0ZXN0LW9ubHk="}},
		{"/response/private", 200, []byte("authorized"), nil, map[string]string{"Authorization": "bAsIc dGVzdDp0ZXN0LW9ubHk="}},
		{"/response/duplicate", 200, []byte("first"), nil, nil},
		{"/", 200, nil, map[string]string{"Content-Type": "text/html"}, nil},
		{"/missing", 404, nil, map[string]string{"X-Common": "server"}, nil},
	}
	for _, c := range cases {
		if err := check(c.path, c.status, c.body, c.headers, c.requestHeaders); err != nil {
			return err
		}
	}
	// Server must remain healthy after the deliberately rejected second send.
	if err := check("/health", 200, []byte(`{"ok":true}`), nil, nil); err != nil {
		return err
	}
	req, _ := http.NewRequest("GET", base+"/response/static-gzip", nil)
	response, err := client.Do(req)
	if err != nil {
		return err
	}
	zipped, err := io.ReadAll(response.Body)
	response.Body.Close()
	if err != nil {
		return err
	}
	if response.StatusCode != 200 || response.Header.Get("Content-Encoding") != "gzip" {
		return fmt.Errorf("static gzip metadata")
	}
	reader, err := gzip.NewReader(bytes.NewReader(zipped))
	if err != nil {
		return err
	}
	decoded, err := io.ReadAll(reader)
	reader.Close()
	if err != nil || string(decoded) != "hello" {
		return fmt.Errorf("static gzip payload")
	}
	checks++
	// Each worker alternates response-only and Request+Response handlers.
	// Keep the stand's external socket budget explicit; never downgrade after failure.
	for _, clients := range concurrentProfiles(maximumClients) {
		var wg sync.WaitGroup
		failures := make(chan error, clients)
		for worker := 0; worker < clients; worker++ {
			wg.Add(1)
			go func(workerIndex int) {
				defer wg.Done()
				for i := 0; i < 30; i++ {
					path, want := "/health", `{"ok":true}`
					if i%2 != 0 {
						path, want = "/echo?value=parallel", "parallel"
					}
					req, _ := http.NewRequest("GET", base+path, nil)
					r, e := client.Do(req)
					if e != nil {
						failures <- fmt.Errorf("parallel clients=%d worker=%d request=%d %s: %w", clients, workerIndex, i+1, path, e)
						return
					}
					b, e := io.ReadAll(r.Body)
					r.Body.Close()
					if e != nil {
						failures <- fmt.Errorf("parallel clients=%d worker=%d request=%d %s body: %w", clients, workerIndex, i+1, path, e)
						return
					}
					if r.StatusCode != 200 || string(b) != want {
						failures <- fmt.Errorf("parallel clients=%d worker=%d request=%d %s response mismatch: status=%d bytes=%d", clients, workerIndex, i+1, path, r.StatusCode, len(b))
						return
					}
				}
			}(worker + 1)
		}
		wg.Wait()
		close(failures)
		for err := range failures {
			return err
		}
		checks += clients * 30
	}
	transport.CloseIdleConnections()
	time.Sleep(300 * time.Millisecond)
	metricResponse, err := client.Get(base + "/response/metrics")
	if err != nil {
		return err
	}
	var metrics struct {
		Heap    uint32 `json:"heap_free"`
		Minimum uint32 `json:"heap_min"`
		Largest uint32 `json:"largest_block"`
		Stack   uint32 `json:"http_stack_min_free"`
	}
	err = json.NewDecoder(metricResponse.Body).Decode(&metrics)
	metricResponse.Body.Close()
	if err != nil || metricResponse.StatusCode != 200 || metrics.Heap < 32768 || metrics.Stack < 512 {
		return fmt.Errorf("HTTP task resource check failed: %+v (%v)", metrics, err)
	}
	fmt.Printf("C3 resource snapshot: free=%d min=%d largest=%d HTTP-stack-min-free=%d bytes\n", metrics.Heap, metrics.Minimum, metrics.Largest, metrics.Stack)
	checks++
	return nil
}

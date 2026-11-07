// Direct LAN acceptance for the real firmware, independent of environment
// proxy settings and speculative HTTP connection pools. No runtime bridge.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"time"
)

const protocol = "2026-07-28"

func main() {
	base := flag.String("base", "", "Controller HTTP origin")
	local := flag.String("local-address", "", "Optional local LAN IPv4 source address")
	flag.Parse()
	if *base == "" {
		panic("-base is required")
	}
	dialer := &net.Dialer{Timeout: 5 * time.Second}
	if *local != "" {
		ip := net.ParseIP(*local)
		if ip == nil {
			panic("invalid local address")
		}
		dialer.LocalAddr = &net.TCPAddr{IP: ip}
	}
	client := &http.Client{Timeout: 5 * time.Second, Transport: &http.Transport{
		Proxy: nil, DialContext: dialer.DialContext, DisableKeepAlives: true,
	}}
	sequence := 0
	rpc := func(method, name string, arguments any, expectedError int) (map[string]any, error) {
		sequence++
		params := map[string]any{"_meta": map[string]any{
			"io.modelcontextprotocol/protocolVersion":    protocol,
			"io.modelcontextprotocol/clientCapabilities": map[string]any{},
		}}
		if name != "" {
			params["name"] = name
			params["arguments"] = arguments
		}
		body, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": sequence, "method": method, "params": params})
		request, err := http.NewRequest(http.MethodPost, *base+"/mcp", bytes.NewReader(body))
		if err != nil {
			return nil, err
		}
		request.SetBasicAuth("demo", "showcase-only") // Existing public demo fixture.
		request.Header.Set("Content-Type", "application/json")
		request.Header.Set("Accept", "application/json, text/event-stream")
		request.Header.Set("Origin", *base)
		request.Header.Set("MCP-Protocol-Version", protocol)
		request.Header.Set("Mcp-Method", method)
		if name != "" {
			request.Header.Set("Mcp-Name", name)
		}
		response, err := client.Do(request)
		if err != nil {
			return nil, err
		}
		defer response.Body.Close()
		data, err := io.ReadAll(io.LimitReader(response.Body, 16385))
		if err != nil {
			return nil, err
		}
		if response.StatusCode != 200 || len(data) > 16384 {
			return nil, fmt.Errorf("%s: HTTP %d", method, response.StatusCode)
		}
		var message struct {
			Version string         `json:"jsonrpc"`
			ID      int            `json:"id"`
			Result  map[string]any `json:"result"`
			Error   *struct {
				Code int `json:"code"`
			} `json:"error"`
		}
		decoder := json.NewDecoder(bytes.NewReader(data))
		decoder.UseNumber()
		if err = decoder.Decode(&message); err != nil {
			return nil, err
		}
		if message.Version != "2.0" || message.ID != sequence {
			return nil, fmt.Errorf("invalid RPC envelope")
		}
		if expectedError != 0 {
			if message.Error == nil || message.Error.Code != expectedError {
				return nil, fmt.Errorf("expected RPC error %d", expectedError)
			}
			return nil, nil
		}
		if message.Error != nil || message.Result["resultType"] != "complete" || message.Result["isError"] == true {
			return nil, fmt.Errorf("RPC/tool error: %s", data)
		}
		return message.Result, nil
	}
	must := func(result map[string]any, err error) map[string]any {
		if err != nil {
			panic(err)
		}
		return result
	}
	text := func(result map[string]any) string {
		return result["content"].([]any)[0].(map[string]any)["text"].(string)
	}
	state := func() map[string]any {
		result := must(rpc("tools/call", "experiment_status", map[string]any{}, 0))
		var value map[string]any
		if err := json.Unmarshal([]byte(text(result)), &value); err != nil {
			panic(err)
		}
		return value
	}
	must(rpc("server/discover", "", nil, 0))
	list := must(rpc("tools/list", "", nil, 0))
	tools := list["tools"].([]any)
	if len(tools) != 4 {
		panic("expected the four full-demo tools")
	}
	must(rpc("tools/call", "device_info", map[string]any{}, 0))
	echo := must(rpc("tools/call", "echo", map[string]any{"text": "Привет, MCP!"}, 0))
	if text(echo) != "Привет, MCP!" {
		panic("UTF-8 echo mismatch")
	}
	if state()["running"] != false {
		panic("another experiment is running; left untouched")
	}
	must(rpc("tools/call", "experiment_set", map[string]any{"action": "start", "duration_ms": 999}, -32602))
	if state()["running"] != false {
		panic("invalid command changed state")
	}
	// Start is bounded even if the response is lost; always attempt explicit stop.
	defer func() {
		if _, err := rpc("tools/call", "experiment_set", map[string]any{"action": "stop"}, 0); err != nil {
			fmt.Fprintln(os.Stderr, "cleanup:", err)
		}
	}()
	must(rpc("tools/call", "experiment_set", map[string]any{
		"action": "start", "kind": "pulse", "duration_ms": 3000,
		"on_ms": 20, "period_ms": 1000, "core": 0, "priority": 1,
	}, 0))
	if state()["running"] != true {
		panic("experiment did not start")
	}
	must(rpc("tools/call", "experiment_set", map[string]any{"action": "stop"}, 0))
	if state()["running"] != false {
		panic("experiment did not stop")
	}
	encoded, _ := json.Marshal(map[string]any{"base": *base, "direct_lan": true, "tools": len(tools), "utf8_echo": true, "invalid_preserves_state": true, "start_status_stop": true, "status": "PASS"})
	fmt.Println(string(encoded))
}

// Network smoke checks for examples/minimal, not a load test or an MCP client.
// Uses public test fixtures; does not read device credentials or change flash.
package main

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"syscall"
	"time"
)

const timeout = 5 * time.Second

func readBody(response *http.Response) (string, error) {
	defer response.Body.Close()
	data, err := io.ReadAll(io.LimitReader(response.Body, 8193))
	if len(data) > 8192 {
		return "", errors.New("HTTP response exceeds smoke-test bound")
	}
	return string(data), err
}

func request(client *http.Client, base, path, method, body, auth string) (int, string, error) {
	req, err := http.NewRequest(method, base+path, strings.NewReader(body))
	if err != nil {
		return 0, "", err
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	}
	if auth != "" {
		req.Header.Set("Authorization", auth)
	}
	response, err := client.Do(req)
	if err != nil {
		return 0, "", err
	}
	data, err := readBody(response)
	if err == nil && response.StatusCode == 401 && !strings.HasPrefix(response.Header.Get("WWW-Authenticate"), "Basic ") {
		return response.StatusCode, data, errors.New("minimal returned 401 without a Basic challenge")
	}
	return response.StatusCode, data, err
}

func expectHTTP(client *http.Client, base, path, method, body, auth string, status int, expected *string) error {
	code, data, err := request(client, base, path, method, body, auth)
	if err != nil {
		return err
	}
	if code != status {
		return fmt.Errorf("%s %s: status %d, expected %d", method, path, code, status)
	}
	if expected != nil && data != *expected {
		return fmt.Errorf("%s %s: unexpected response body", method, path)
	}
	return nil
}

func text(value string) *string { return &value }

func writeAll(writer io.Writer, data []byte) error {
	for len(data) != 0 {
		n, err := writer.Write(data)
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		data = data[n:]
	}
	return nil
}

func partialPost(host string) error {
	conn, err := net.DialTimeout("tcp", host, timeout)
	if err != nil {
		return err
	}
	defer conn.Close()
	if err = conn.SetDeadline(time.Now().Add(timeout)); err != nil {
		return err
	}
	body := "value=partial+post"
	header := fmt.Sprintf("POST /echo HTTP/1.1\r\nHost: %s\r\nContent-Length: %d\r\nContent-Type: application/x-www-form-urlencoded\r\nConnection: close\r\n\r\n", host, len(body))
	if err = writeAll(conn, []byte(header)); err != nil {
		return err
	}
	for _, part := range []string{body[:4], body[4:9], body[9:]} {
		if err = writeAll(conn, []byte(part)); err != nil {
			return err
		}
		time.Sleep(80 * time.Millisecond)
	}
	response, err := http.ReadResponse(bufio.NewReader(conn), nil)
	if err != nil {
		return err
	}
	data, err := readBody(response)
	if err != nil {
		return err
	}
	if response.StatusCode != 200 || data != "partial post" {
		return errors.New("partial POST response mismatch")
	}
	return nil
}

// The reader accepts only the complete, unmasked server frames exercised here.
// It is intentionally not a general WebSocket implementation.
func readFrame(reader io.Reader) (byte, []byte, error) {
	var header [2]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return 0, nil, err
	}
	if header[0]&0xf0 != 0x80 || header[1]&0x80 != 0 {
		return 0, nil, errors.New("fragmented, reserved, or masked server frame")
	}
	size := uint64(header[1] & 127)
	if size == 126 {
		var extended [2]byte
		if _, err := io.ReadFull(reader, extended[:]); err != nil {
			return 0, nil, err
		}
		size = uint64(binary.BigEndian.Uint16(extended[:]))
	} else if size == 127 {
		var extended [8]byte
		if _, err := io.ReadFull(reader, extended[:]); err != nil {
			return 0, nil, err
		}
		size = binary.BigEndian.Uint64(extended[:])
	}
	if size > 4096 {
		return 0, nil, errors.New("server frame exceeds 4096 bytes")
	}
	data := make([]byte, int(size))
	_, err := io.ReadFull(reader, data)
	return header[0] & 15, data, err
}

func encodeFrame(opcode byte, data []byte, mask [4]byte) ([]byte, error) {
	if len(data) > 4096 {
		return nil, errors.New("smoke frame exceeds 4096 bytes")
	}
	frame := []byte{0x80 | opcode}
	if len(data) < 126 {
		frame = append(frame, 0x80|byte(len(data)))
	} else {
		frame = append(frame, 0xfe, byte(len(data)>>8), byte(len(data)))
	}
	frame = append(frame, mask[:]...)
	for i, value := range data {
		frame = append(frame, value^mask[i%4])
	}
	return frame, nil
}

func sendFrame(conn net.Conn, opcode byte, data []byte) error {
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	frame, err := encodeFrame(opcode, data, mask)
	if err != nil {
		return err
	}
	return writeAll(conn, frame)
}

func websocket(host string) (net.Conn, *bufio.Reader, error) {
	conn, err := net.DialTimeout("tcp", host, timeout)
	if err != nil {
		return nil, nil, err
	}
	success := false
	defer func() {
		if !success {
			conn.Close()
		}
	}()
	if err = conn.SetDeadline(time.Now().Add(timeout)); err != nil {
		return nil, nil, err
	}
	var nonce [16]byte
	if _, err = rand.Read(nonce[:]); err != nil {
		return nil, nil, err
	}
	key := base64.StdEncoding.EncodeToString(nonce[:])
	header := fmt.Sprintf("GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", host, key)
	if err = writeAll(conn, []byte(header)); err != nil {
		return nil, nil, err
	}
	reader := bufio.NewReader(conn)
	response, err := http.ReadResponse(reader, nil)
	if err != nil {
		return nil, nil, err
	}
	accept := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	if response.StatusCode != 101 || response.Header.Get("Sec-WebSocket-Accept") != base64.StdEncoding.EncodeToString(accept[:]) ||
		!strings.EqualFold(response.Header.Get("Upgrade"), "websocket") {
		return nil, nil, errors.New("invalid WebSocket handshake")
	}
	success = true
	return conn, reader, nil
}

func echo(conn net.Conn, reader io.Reader, opcode byte, data []byte) error {
	if err := conn.SetDeadline(time.Now().Add(timeout)); err != nil {
		return err
	}
	if err := sendFrame(conn, opcode, data); err != nil {
		return err
	}
	for {
		op, received, err := readFrame(reader)
		if err != nil {
			return err
		}
		if op == 1 && string(received) == "tick" {
			continue
		}
		if op != opcode || !bytes.Equal(data, received) {
			return errors.New("WebSocket echo mismatch")
		}
		return nil
	}
}

func run(base, host string) error {
	transport := &http.Transport{DisableKeepAlives: true}
	defer transport.CloseIdleConnections()
	client := &http.Client{Timeout: timeout, Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	valid := base64.StdEncoding.EncodeToString([]byte("test:test-only"))
	checks := []struct {
		path, method, body, auth string
		status                   int
		expected                 *string
	}{
		{"/health", "GET", "", "", 200, text(`{"ok":true}`)},
		{"/missing", "GET", "", "", 404, nil},
		{"/echo?value=a%20b%2Bc", "GET", "", "", 200, text("a b+c")},
		{"/echo", "POST", "value=post+body", "", 200, text("post body")},
		{"/private", "GET", "", "", 401, nil},
		{"/private", "GET", "", "Basic " + valid, 200, text("authorized")},
		{"/private", "DELETE", "", "", 401, nil},
		{"/private", "DELETE", "", "Basic " + valid, 200, text("authorized")},
	}
	for _, check := range checks {
		if err := expectHTTP(client, base, check.path, check.method, check.body, check.auth, check.status, check.expected); err != nil {
			return err
		}
	}
	for _, bad := range []string{"", valid[:4], valid[:len(valid)-1], valid + "x", base64.StdEncoding.EncodeToString([]byte("test:wrong"))} {
		if err := expectHTTP(client, base, "/private", "GET", "", "Basic "+bad, 401, nil); err != nil {
			return err
		}
	}
	fmt.Println("PASS HTTP routes, forms, Basic rejection/success and DELETE")
	if err := partialPost(host); err != nil {
		return err
	}
	status, _, err := request(client, base, "/echo", "POST", strings.Repeat("x", 4097), "")
	if err != nil && !errors.Is(err, io.EOF) && !errors.Is(err, io.ErrUnexpectedEOF) && !errors.Is(err, syscall.ECONNRESET) && !errors.Is(err, syscall.EPIPE) {
		return err
	}
	if err == nil && status != 400 {
		return fmt.Errorf("oversized body status: %d", status)
	}
	if err = expectHTTP(client, base, "/health", "GET", "", "", 200, text(`{"ok":true}`)); err != nil {
		return err
	}
	fmt.Println("PASS partial POST, body limit and subsequent HTTP health")
	if err := checkBodyAPI(client, base); err != nil {
		return err
	}
	conn, reader, err := websocket(host)
	if err != nil {
		return err
	}
	defer conn.Close()
	for _, size := range []int{0, 1, 127, 128, 256, 4096} {
		for _, opcode := range []byte{1, 2} {
			data := bytes.Repeat([]byte{'x'}, size)
			if opcode == 2 {
				for i := range data {
					data[i] = byte(i)
				}
			}
			if err = echo(conn, reader, opcode, data); err != nil {
				return err
			}
		}
	}
	_ = sendFrame(conn, 8, nil)
	conn.Close()
	for i := 0; i < 5; i++ {
		reconnected, input, err := websocket(host)
		if err != nil {
			return err
		}
		err = echo(reconnected, input, 1, []byte("reconnect"))
		_ = sendFrame(reconnected, 8, nil)
		reconnected.Close()
		if err != nil {
			return err
		}
	}
	if err = expectHTTP(client, base, "/health", "GET", "", "", 200, text(`{"ok":true}`)); err != nil {
		return err
	}
	fmt.Println("PASS WS text/binary 0..4096, five reconnects, HTTP after WS")
	return nil
}

func target(value string) (string, string, error) {
	if !strings.Contains(value, "://") {
		value = "http://" + value
	}
	u, err := url.Parse(value)
	if err != nil {
		return "", "", errors.New("invalid HTTP target")
	}
	if u.Scheme != "http" || u.Hostname() == "" || u.User != nil || (u.Path != "" && u.Path != "/") || u.RawQuery != "" || u.Fragment != "" {
		return "", "", errors.New("expected an HTTP host[:port], without credentials or path")
	}
	port := u.Port()
	if port == "" {
		port = "80"
	}
	return "http://" + u.Host, net.JoinHostPort(u.Hostname(), port), nil
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "Usage: go run . HOST[:PORT]  (examples/minimal only)")
		os.Exit(2)
	}
	base, host, err := target(os.Args[1])
	if err == nil {
		err = run(base, host)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "FAIL:", err)
		os.Exit(1)
	}
}

package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"path/filepath"
)

func write(directory, name string, data []byte) {
	if err := os.MkdirAll(directory, 0755); err != nil {
		panic(err)
	}
	if err := os.WriteFile(filepath.Join(directory, name), data, 0644); err != nil {
		panic(err)
	}
}

func frame(opcode byte, final bool, payload []byte) []byte {
	first := opcode
	if final {
		first |= 0x80
	}
	result := []byte{first}
	switch {
	case len(payload) < 126:
		result = append(result, 0x80|byte(len(payload)))
	case len(payload) <= 65535:
		result = append(result, 0xfe, byte(len(payload)>>8), byte(len(payload)))
	default:
		panic("bounded seed too large")
	}
	result = append(result, 1, 2, 3, 4)
	for index, value := range payload {
		result = append(result, value^byte(1+index%4))
	}
	return result
}

func dictionary(tokens [][]byte) []byte {
	var output bytes.Buffer
	for index, token := range tokens {
		fmt.Fprintf(&output, "token%d=\"", index)
		for _, value := range token {
			fmt.Fprintf(&output, "\\x%02x", value)
		}
		fmt.Fprintln(&output, "\"")
	}
	return output.Bytes()
}

func main() {
	output := flag.String("out", "local/tcp-p5-fuzz/corpus", "generated seed root; existing unrelated files are preserved")
	flag.Parse()
	http := filepath.Join(*output, "http")
	websocket := filepath.Join(*output, "websocket")
	requests := map[string]string{
		"get":          "GET /health HTTP/1.1\r\nHost: test\r\n\r\n",
		"head":         "HEAD / HTTP/1.1\r\nHost: test\r\n\r\n",
		"http10":       "GET / HTTP/1.0\r\n\r\n",
		"post":         "POST /payload HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\nbody",
		"duplicate_cl": "POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
		"cl_te":        "POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx",
		"folded":       "GET / HTTP/1.1\r\nHost: test\r\nX-Test: value\r\n continued\r\n\r\n",
		"bare_lf":      "GET / HTTP/1.1\nHost: test\n\n",
		"upgrade":      "GET /ws HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
		"expect":       "POST / HTTP/1.1\r\nHost: test\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\nx",
	}
	for name, request := range requests {
		write(http, name, []byte(request))
	}
	write(http, "target_1025", append(append([]byte("GET /"), bytes.Repeat([]byte{'a'}, 1024)...), []byte(" HTTP/1.1\r\nHost: test\r\n\r\n")...))
	write(http, "header_8192", bytes.Repeat([]byte{'A'}, 8192))
	write(http, "embedded_nul", []byte("GET / HTTP/1.1\r\nHost: test\x00\r\n\r\n"))
	write(http, "writer_empty204", []byte{0x11, 2, 0})
	write(http, "writer_empty304", []byte{0x11, 3, 0})
	write(websocket, "text", frame(1, true, []byte("hello")))
	write(websocket, "utf8", frame(1, true, []byte("привет 😀")))
	write(websocket, "ping_pong", append(frame(9, true, []byte("ping")), frame(10, true, []byte("pong"))...))
	write(websocket, "close", frame(8, true, []byte{3, 232}))
	write(websocket, "bad_close", frame(8, true, []byte{3, 237}))
	write(websocket, "invalid_utf8", frame(1, true, []byte{0xc0, 0xaf}))
	write(websocket, "fragment_utf8", append(frame(1, false, []byte{0xf0, 0x9f}), frame(0, true, []byte{0x98, 0x80})...))
	write(websocket, "fragment4096", append(frame(2, false, bytes.Repeat([]byte{0xa7}, 2048)), frame(0, true, bytes.Repeat([]byte{0xa7}, 2048))...))
	write(websocket, "message4097", frame(2, true, bytes.Repeat([]byte{0xa7}, 4097)))
	write(websocket, "unmasked", []byte{0x81, 0})
	write(websocket, "noncanonical126", []byte{0x81, 0xfe, 0, 1, 1, 2, 3, 4, 'a' ^ 1})
	write(websocket, "highbit127", []byte{0x82, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4})
	write(websocket, "key", []byte("dGhlIHNhbXBsZSBub25jZQ=="))
	write(*output, "http.dict", dictionary([][]byte{[]byte("GET "), []byte("POST "), []byte("HTTP/1.1\r\n"), []byte("\r\n\r\n"), []byte("Host: "), []byte("Content-Length: "), []byte("Transfer-Encoding: "), []byte("Connection: "), []byte("Upgrade: websocket"), []byte("Sec-WebSocket-Key: ")}))
	write(*output, "websocket.dict", dictionary([][]byte{{0x81, 0x80}, {0x82, 0xfe}, {0x82, 0xff}, {0x89, 0x80}, {0x88, 0x82}, {1, 2, 3, 4}, {0x03, 0xe8}}))
	fmt.Printf("FUZZ_CORPUS generated=%s http=15 websocket=13\n", *output)
}

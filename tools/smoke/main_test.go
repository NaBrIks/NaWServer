package main

import (
	"bytes"
	"encoding/binary"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestFrameBoundaries(t *testing.T) {
	for _, size := range []int{0, 1, 125, 126, 127, 128, 256, 4096} {
		data := bytes.Repeat([]byte{0x5a}, size)
		mask := [4]byte{1, 2, 3, 4}
		wire, err := encodeFrame(2, data, mask)
		if err != nil {
			t.Fatal(err)
		}
		if wire[0] != 0x82 || wire[1]&0x80 == 0 {
			t.Fatal("incorrect opcode/mask")
		}
		offset := 2
		if size >= 126 {
			if wire[1] != 0xfe || int(binary.BigEndian.Uint16(wire[2:4])) != size {
				t.Fatal("incorrect extended length")
			}
			offset = 4
		} else if int(wire[1]&127) != size {
			t.Fatal("incorrect length")
		}
		if !bytes.Equal(wire[offset:offset+4], mask[:]) {
			t.Fatal("incorrect mask")
		}
		for i, value := range wire[offset+4:] {
			if value^mask[i%4] != data[i] {
				t.Fatal("mask mismatch")
			}
		}
		// Build an independent server frame; the server must not mask its data.
		server := []byte{0x82, byte(size)}
		if size >= 126 {
			server = []byte{0x82, 126, byte(size >> 8), byte(size)}
		}
		server = append(server, data...)
		opcode, received, err := readFrame(bytes.NewReader(server))
		if err != nil || opcode != 2 || !bytes.Equal(received, data) {
			t.Fatalf("decode %d: %v", size, err)
		}
	}
}

func TestRejectMalformedFrames(t *testing.T) {
	for _, wire := range [][]byte{
		{0x02, 0}, {0xc2, 0}, {0x82, 0x80}, {0x82, 126, 0x10, 1},
		{0x82, 127, 0, 0, 0, 1, 0, 0, 0, 0}, {0x82, 126, 0}, {0x82, 2, 1},
	} {
		if _, _, err := readFrame(bytes.NewReader(wire)); err == nil {
			t.Fatalf("accepted malformed frame %x", wire)
		}
	}
	if _, err := encodeFrame(2, make([]byte, 4097), [4]byte{}); err == nil {
		t.Fatal("accepted oversized client frame")
	}
}

type shortWriter struct{ bytes.Buffer }

func (w *shortWriter) Write(data []byte) (int, error) {
	if len(data) > 2 {
		data = data[:2]
	}
	return w.Buffer.Write(data)
}

type zeroWriter struct{}

func (zeroWriter) Write([]byte) (int, error) { return 0, nil }
func TestPartialWrites(t *testing.T) {
	w := &shortWriter{}
	if err := writeAll(w, []byte("abcdef")); err != nil || w.String() != "abcdef" {
		t.Fatal("partial write lost data")
	}
	if err := writeAll(zeroWriter{}, []byte("x")); err != io.ErrShortWrite {
		t.Fatal("zero write not rejected")
	}
}

func TestTargets(t *testing.T) {
	for input, expected := range map[string]string{"localhost": "localhost:80", "http://localhost:8080/": "localhost:8080", "[::1]:8080": "[::1]:8080"} {
		_, host, err := target(input)
		if err != nil || host != expected {
			t.Fatalf("%s: %s %v", input, host, err)
		}
	}
	for _, input := range []string{"https://localhost", "http://a:b@localhost", "http://localhost/path", "http://localhost?x=1", ""} {
		if _, _, err := target(input); err == nil {
			t.Fatalf("accepted invalid target %q", input)
		}
	}
	if _, _, err := target("http://user:PRIVATE%broken@localhost"); err == nil || strings.Contains(err.Error(), "PRIVATE") {
		t.Fatal("invalid target error may expose credentials")
	}
}

func TestBasicChallenge(t *testing.T) {
	for _, challenge := range []string{"", "Bearer", `Basic realm="fixture"`} {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("WWW-Authenticate", challenge)
			w.WriteHeader(401)
		}))
		_, _, err := request(server.Client(), server.URL, "/private", "GET", "", "")
		server.Close()
		if (err == nil) != strings.HasPrefix(challenge, "Basic ") {
			t.Fatal("incorrect Basic challenge validation")
		}
	}
}

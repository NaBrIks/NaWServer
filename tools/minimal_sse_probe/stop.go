package main

import (
	"bufio"
	"bytes"
	"crypto/sha1"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"syscall"
	"time"
)

// Only a minimal text/tick witness, not a general WebSocket client. It proves
// the old WS connection closes across /events/stop, then a new one can echo.
type witness struct {
	conn   net.Conn
	reader *bufio.Reader
	limit  time.Time
}

func (p *probe) openWitness() (*witness, error) {
	connection, err := p.dial()
	if err != nil {
		return nil, fmt.Errorf("WS witness dial: %w", err)
	}
	success := false
	defer func() {
		if !success {
			connection.Close()
		}
	}()
	request, err := http.NewRequest("GET", p.base+"/ws", nil)
	if err != nil {
		return nil, err
	}
	const key = "MDEyMzQ1Njc4OWFiY2RlZg=="
	request.Header.Set("Upgrade", "websocket")
	request.Header.Set("Connection", "Upgrade")
	request.Header.Set("Sec-WebSocket-Version", "13")
	request.Header.Set("Sec-WebSocket-Key", key)
	if err = request.Write(connection); err != nil {
		return nil, fmt.Errorf("WS witness write handshake: %w", err)
	}
	response, reader, err := guardedResponse(connection, request)
	if err != nil {
		return nil, fmt.Errorf("WS witness response headers: %w", err)
	}
	accept := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	if response.StatusCode != 101 || response.Header.Get("Upgrade") != "websocket" ||
		response.Header.Get("Sec-WebSocket-Accept") != base64.StdEncoding.EncodeToString(accept[:]) {
		return nil, errors.New("restart WS witness handshake mismatch")
	}
	if err = connection.SetDeadline(time.Time{}); err != nil {
		return nil, err
	}
	success = true
	limit, _ := p.ctx.Deadline()
	return &witness{connection, reader, limit}, nil
}

func writeAll(writer io.Writer, bytes []byte) error {
	for len(bytes) != 0 {
		n, err := writer.Write(bytes)
		if err != nil {
			return err
		}
		if n <= 0 || n > len(bytes) {
			return io.ErrShortWrite
		}
		bytes = bytes[n:]
	}
	return nil
}

func witnessFrame(reader io.Reader) (byte, []byte, error) {
	var header [2]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return 0, nil, err
	}
	if header[0]&0xf0 != 0x80 || header[1]&0x80 != 0 || header[1] > 125 {
		return 0, nil, errors.New("unexpected restart witness frame")
	}
	payload := make([]byte, int(header[1]))
	_, err := io.ReadFull(reader, payload)
	return header[0] & 15, payload, err
}

func (w *witness) echo(text string) error {
	if len(text) > 125 {
		return errors.New("witness payload exceeds bound")
	}
	if err := w.conn.SetDeadline(earlier(time.Now().Add(operationTimeout), w.limit)); err != nil {
		return err
	}
	// RFC6455 requires a client mask; this fixed test mask is not a security RNG.
	mask := [4]byte{1, 2, 3, 4}
	frame := []byte{0x81, 0x80 | byte(len(text)), 1, 2, 3, 4}
	for index := range text {
		frame = append(frame, text[index]^mask[index%4])
	}
	if err := writeAll(w.conn, frame); err != nil {
		return fmt.Errorf("WS witness write echo: %w", err)
	}
	for count := 0; count < 4; count++ {
		opcode, payload, err := witnessFrame(w.reader)
		if err != nil {
			return fmt.Errorf("WS witness read echo frame%d: %w", count+1, err)
		}
		if opcode == 1 && bytes.Equal(payload, []byte(text)) {
			return nil
		}
		if opcode != 1 || string(payload) != "tick" {
			return errors.New("restart witness echo mismatch")
		}
	}
	return errors.New("too many tick frames before witness echo")
}

func (w *witness) closed(deadline time.Time) error {
	if err := w.conn.SetDeadline(earlier(deadline, w.limit)); err != nil {
		return err
	}
	for count := 0; count < 16; count++ {
		opcode, payload, err := witnessFrame(w.reader)
		if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, syscall.ECONNRESET) {
			return nil
		}
		if err != nil {
			return fmt.Errorf("old WS did not close across server stop: %w", err)
		}
		if opcode == 8 {
			// A server may send a small Close before closing TCP. Echo it masked;
			// still require the actual TCP close, rather than accepting the frame.
			frame := []byte{0x88, 0x80 | byte(len(payload)), 1, 2, 3, 4}
			for index, value := range payload {
				frame = append(frame, value^byte(index%4+1))
			}
			if err := writeAll(w.conn, frame); err != nil {
				if errors.Is(err, syscall.ECONNRESET) || errors.Is(err, syscall.EPIPE) {
					return nil
				}
				return err
			}
			continue
		}
		if opcode != 1 || string(payload) != "tick" {
			return errors.New("unexpected frame while waiting for old WS close")
		}
	}
	return errors.New("old WS exceeded bounded stop frame count")
}

func (p *probe) stopStage(before stats, clients int) error {
	old, err := p.openWitness()
	if err != nil {
		return fmt.Errorf("open old witness: %w", err)
	}
	defer old.conn.Close()
	if err = p.health(); err != nil {
		return fmt.Errorf("HTTP health with idle old WS: %w", err)
	}
	var streams [4]*stream
	for index := 0; index < clients; index++ {
		streams[index], err = p.open("/events", "", nil)
		if err != nil {
			return fmt.Errorf("SSE%d open: %w", index+1, err)
		}
		defer streams[index].conn.Close()
		if err = streams[index].connected("ready"); err != nil {
			return fmt.Errorf("SSE%d connected event: %w", index+1, err)
		}
	}
	if err = p.publish("probe:stop", "queued before stop"); err != nil {
		return fmt.Errorf("publish before stop: %w", err)
	}
	if err = old.echo("before-stop"); err != nil {
		return fmt.Errorf("old witness before stop: %w", err)
	}
	currentStats, err := p.state()
	if err != nil {
		return fmt.Errorf("%d-active pre-stop stats: %w", clients, err)
	}
	if !activeBeforeStop(before, currentStats, clients) {
		return fmt.Errorf("requested %d SSE streams were not still active immediately before stop", clients)
	}
	status, _, body, err := p.request("POST", "/events/stop", "", "")
	if err != nil {
		return fmt.Errorf("request stop: %w", err)
	}
	if status != 202 || string(body) != "stop requested" {
		return errors.New("stop request not acknowledged")
	}
	// The acknowledgement is not completion. Observe all native lifetimes and
	// counters, then prove the new listener also admits a working WS session.
	deadline := p.deadline(drainTimeout)
	for index := 0; index < clients; index++ {
		if err = closedStream(streams[index], deadline); err != nil {
			return fmt.Errorf("SSE%d closed after stop: %w", index+1, err)
		}
	}
	if err = old.closed(deadline); err != nil {
		return fmt.Errorf("old witness closed after stop: %w", err)
	}
	for {
		if err = p.health(); err == nil {
			break
		}
		if !time.Now().Before(deadline) {
			return fmt.Errorf("HTTP did not recover after stop: %w", err)
		}
		if err := p.pause(); err != nil {
			return err
		}
	}
	if _, err = p.waitDrain(before, uint64(clients), false); err != nil {
		return fmt.Errorf("drain %d after stop: %w", clients, err)
	}
	current, err := p.openWitness()
	if err != nil {
		return fmt.Errorf("open new witness after stop: %w", err)
	}
	defer current.conn.Close()
	if err := current.echo("after-stop"); err != nil {
		return fmt.Errorf("new witness after stop: %w", err)
	}
	return nil
}

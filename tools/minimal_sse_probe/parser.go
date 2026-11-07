package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	maxBodyBytes  = 4096
	maxHeadBytes  = 8192
	maxLineBytes  = 1024
	maxEventBytes = 4096
	maxEventLines = 64
)

// Guards only the HTTP head, including its delimiter. Coalesced body bytes do
// not count against that bound. http.ReadResponse still decodes chunked bodies.
type headGuard struct {
	source                    io.Reader
	total, line, lines, match int
	previous                  byte
	done                      bool
}

func (g *headGuard) Read(destination []byte) (int, error) {
	if !g.done && len(destination) > maxHeadBytes-g.total+1 {
		destination = destination[:maxHeadBytes-g.total+1]
	}
	n, err := g.source.Read(destination)
	if g.done {
		return n, err
	}
	const delimiter = "\r\n\r\n"
	for _, b := range destination[:n] {
		g.total++
		g.line++
		if g.total > maxHeadBytes || g.line > maxLineBytes {
			return 0, errors.New("HTTP head/line exceeds probe bound")
		}
		if b == '\n' {
			if g.previous != '\r' {
				return 0, errors.New("HTTP head contains bare LF")
			}
			g.lines++
			g.line = 0
			if g.lines > 34 { // Status, at most 32 fields, final empty line.
				return 0, errors.New("HTTP head has too many lines")
			}
		}
		g.previous = b
		if b == delimiter[g.match] {
			g.match++
		} else if b == delimiter[0] {
			g.match = 1
		} else {
			g.match = 0
		}
		if g.match == len(delimiter) {
			g.done = true
			break
		}
	}
	return n, err
}

type event struct {
	Name, Data, ID string
	Retry          uint64
}

func readEvent(reader *bufio.Reader, previousID string) (event, error) {
	result := event{Name: "message", ID: previousID}
	var data []string
	seenData, total := false, 0
	for lines := 0; lines < maxEventLines; lines++ {
		line, err := reader.ReadSlice('\n')
		if err != nil {
			return event{}, fmt.Errorf("SSE line incomplete or over bound: %w", err)
		}
		total += len(line)
		if len(line) > maxLineBytes || total > maxEventBytes {
			return event{}, errors.New("SSE event exceeds probe bound")
		}
		line = bytes.TrimSuffix(bytes.TrimSuffix(line, []byte{'\n'}), []byte{'\r'})
		if len(line) == 0 {
			if !seenData {
				continue
			}
			result.Data = strings.Join(data, "\n")
			if !utf8.ValidString(result.Data) || !utf8.ValidString(result.ID) {
				return event{}, errors.New("SSE event contains invalid UTF-8")
			}
			return result, nil
		}
		if line[0] == ':' {
			continue
		}
		key, value, _ := strings.Cut(string(line), ":")
		value = strings.TrimPrefix(value, " ")
		switch key {
		case "data":
			seenData = true
			data = append(data, value)
		case "event":
			if value != "" {
				result.Name = value
			}
		case "id":
			if strings.ContainsRune(value, 0) {
				return event{}, errors.New("SSE id contains NUL")
			}
			result.ID = value
		case "retry":
			if value == "" || strings.IndexFunc(value, func(r rune) bool { return r < '0' || r > '9' }) >= 0 {
				return event{}, errors.New("invalid SSE retry")
			}
			retry, err := strconv.ParseUint(value, 10, 32)
			if err != nil {
				return event{}, errors.New("SSE retry overflows uint32")
			}
			result.Retry = retry
		}
	}
	return event{}, errors.New("SSE event has too many lines")
}

type stats struct {
	Active    uint64 `json:"active"`
	Begun     uint64 `json:"begun"`
	Completed uint64 `json:"completed"`
	Queued    uint64 `json:"queued_bytes"`
}

func parseStats(data []byte) (stats, error) {
	var wire struct {
		Active, Begun, Completed *uint64
		Queued                   *uint64 `json:"queued_bytes"`
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	if err := decoder.Decode(&wire); err != nil {
		return stats{}, err
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF || wire.Active == nil || wire.Begun == nil ||
		wire.Completed == nil || wire.Queued == nil {
		return stats{}, errors.New("stats missing required counters or trailing JSON")
	}
	result := stats{*wire.Active, *wire.Begun, *wire.Completed, *wire.Queued}
	if result.Active > 4 || result.Completed > result.Begun {
		return stats{}, errors.New("invalid minimal SSE counters")
	}
	return result, nil
}

func drained(before, current stats, admitted uint64) (bool, error) {
	if before.Begun > ^uint64(0)-admitted || before.Completed > ^uint64(0)-admitted {
		return false, errors.New("counter addition overflow")
	}
	if current.Begun != before.Begun+admitted || current.Completed < before.Completed ||
		current.Completed > before.Completed+admitted {
		return false, errors.New("unexpected SSE admission/completion counters; unrelated clients or restart")
	}
	return current.Active == 0 && current.Queued == 0 && current.Completed == before.Completed+admitted, nil
}

func validClients(clients int) bool {
	return clients >= 1 && clients <= 4
}

func activeBeforeStop(before, current stats, clients int) bool {
	return validClients(clients) && before.Begun <= ^uint64(0)-uint64(clients) &&
		current.Active == uint64(clients) && current.Begun == before.Begun+uint64(clients) &&
		current.Completed == before.Completed
}

func boundedBody(reader io.Reader) ([]byte, error) {
	data, err := io.ReadAll(io.LimitReader(reader, maxBodyBytes+1))
	if len(data) > maxBodyBytes {
		return nil, errors.New("HTTP body exceeds probe bound")
	}
	return data, err
}

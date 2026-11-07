package main

import (
	"fmt"
	"time"
)

// Diagnostic control: use exactly the regular p.open/connected calls and raw
// deadlines, but hold no WS witness. This is not the stop/restart fixture.
func (p *probe) admissionStage(before stats, clients int) error {
	var streams [4]*stream
	for index := 0; index < clients; index++ {
		var err error
		streams[index], err = p.open("/events", "", nil)
		if err != nil {
			return fmt.Errorf("SSE%d open: %w", index+1, err)
		}
		defer streams[index].conn.Close()
		if err = streams[index].connected("ready"); err != nil {
			return fmt.Errorf("SSE%d connected event: %w", index+1, err)
		}
	}
	current, err := p.state()
	if err != nil {
		return fmt.Errorf("%d-active stats: %w", clients, err)
	}
	if !activeBeforeStop(before, current, clients) {
		return fmt.Errorf("requested %d SSE streams not simultaneously active with exact counters", clients)
	}
	if _, err = p.waitDrain(before, uint64(clients), true); err != nil {
		return fmt.Errorf("close/drain %d: %w", clients, err)
	}
	deadline := earlier(time.Now().Add(operationTimeout), p.deadline(drainTimeout))
	for index := 0; index < clients; index++ {
		if err = closedStream(streams[index], deadline); err != nil {
			return fmt.Errorf("SSE%d closed after drain: %w", index+1, err)
		}
	}
	return nil
}

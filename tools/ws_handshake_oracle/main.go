// Independent test oracle; these deterministic nonces are not credentials.
package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"flag"
	"fmt"
	"os"
)

func main() {
	count := flag.Int("count", 1024, "number of deterministic test vectors")
	seed := flag.Uint64("seed", 0x64553174, "deterministic xorshift seed")
	flag.Parse()
	if *count < 1 || *count > 100000 || *seed == 0 {
		fmt.Fprintln(os.Stderr, "count must be 1..100000 and seed nonzero")
		os.Exit(2)
	}
	writer := bufio.NewWriter(os.Stdout)
	state := *seed
	for index := 0; index < *count; index++ {
		var nonce [16]byte
		for offset := range nonce {
			state ^= state << 13
			state ^= state >> 7
			state ^= state << 17
			nonce[offset] = byte(state)
		}
		key := base64.StdEncoding.EncodeToString(nonce[:])
		digest := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
		accept := base64.StdEncoding.EncodeToString(digest[:])
		if _, err := fmt.Fprintf(writer, "%s\t%s\n", key, accept); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	if err := writer.Flush(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

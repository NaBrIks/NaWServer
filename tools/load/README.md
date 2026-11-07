# Mixed LAN load harness

[Short transmission scenarios](transmission-scenarios.md) provide a separate
Node.js tool for bounded idle/API/WS/SSE/asset comparisons, cumulative task CPU
measurements and explicit sent/replied/pending echo accounting.

Measurement limitation for the recorded night run: `ws_echo_sent` and
`ws_echo_received` are incremented together after a verified exchange, so they
count completed exact echoes rather than all attempted sends. Failed exchanges
remain in `errors`/`timeouts`; equality of the two counters is not a delivery-rate
proof. `body_integrity_failures` covers HTTP validation, while WS decode/echo
errors enter the general error counters. The bounded reason map can group
different failures into `other_capped`. Preserve these definitions when comparing
the saved runs; do not relabel them as zero-loss acceptance.

This Go standard-library tool runs a bounded, resumable mixed-protocol stage against a NaWServer controller. It is intended for the planned two- and four-client MCU profiles, not throughput benchmarking. It changes no device state: MCP checks list tools, call read-only `echo`, and submit invalid `echo` arguments; WebSocket traffic is the application's CBOR echo contract; SSE is read-only `/api/v2/events` progress; HTTP reads hashed assets referenced from `/` and `/api/v2/summary`.

## Run

Normal MCU traffic is separate from the historical mixed stress below:

```powershell
# Automation: 1/2/3 workers, one outstanding REST read each, 1 request/s/worker.
go -C tools/load run . -profile rest -clients 3 -interval 1s -duration 60s -base http://DEVICE -local-address LOCAL_IPV4 -out C:/absolute/ignored/rest3.jsonl
# HTTP UI visit: fetch assets once per client, then read status/info; no WS/SSE.
go -C tools/load run . -profile ui -clients 2 -interval 1s -duration 60s -base http://DEVICE -local-address LOCAL_IPV4 -out C:/absolute/ignored/ui2.jsonl
# Negative limits: supply the configured header cap (C6 fixture8192, S3 fixture2048).
go -C tools/load run . -profile limits -http-header-limit 2048 -base http://DEVICE -local-address LOCAL_IPV4 -out C:/absolute/ignored/limits.json
```

Normal runs require fresh output files, accept only1–3 clients and intervals1–10s.
REST reads summary nine times then INFO once in each ten-request cycle, checks
boot identity and performs final health/info recovery. It sends no assets, MCP,
WS/SSE or mutation requests. UI mode is an HTTP visit scenario, **not a browser
E2E or a live WS/SSE client**. These profiles do not turn a sixty-second check
into proof of continuous overnight automation reliability. Background owner
connections are preserved and must be recorded when interpreting admission.
Limits mode checks oversized body (existing dispatcher HTTP400), headers431,
WS admission503 and oversized WS close1009; it closes only its owned sockets.
An initial probe with an incorrect expected413/header cap must be retained as
a harness failure, not evidence that an oversized body was accepted.

Default `-profile stress` retains the previous mixed load and thresholds.

Run from the repository root. `go -C` selects the nested module, and output paths are relative to `tools/load/`:

```powershell
go -C tools/load run . -base http://<verified-device-ip> -clients 2 -duration 15m -out normal-2.jsonl
go -C tools/load run . -base http://<verified-device-ip> -clients 4 -duration 30m -out mixed-4.jsonl
```

An orchestrator can pass `-stop-file <path>`; when that file appears the client stops within about one second, closes/drains sockets and writes the recovery record. Each sample also records up to 12 distinct bounded error reasons plus the latest reason.

Replace the placeholder with the verified address of the selected device. The source address defaults to `192.0.2.3`. The tool binds that local IPv4 address for both HTTP and raw protocol sockets, disables proxies, uses HTTP only, and rejects a base URL with a path, credentials, or port 443. Never change the controller's configured port as part of this run.

`-clients` accepts only `2` or `4`. It holds half the logical clients as WebSocket clients and half as SSE clients. Every logical client has one serial HTTP worker. The four-client profile therefore intends at most two WS + two SSE + three pooled HTTP sockets (seven total). The HTTP pool includes keep-alive sockets and is explicitly capped at three. A browser or another local client may already occupy device slots: admission errors are recorded and the harness leaves other clients alone. Do not interpret a 503 or failed upgrade as a harness cleanup request.

The per-client HTTP and WS cadence defaults to `-interval 250ms` (allowed range 100ms–10s). At startup the tool reads `/`, discovers referenced `/assets/*.js` and `/assets/*.css`, then fetches those actual bundles; `-asset /path` adds an explicit resource. It handles the firmware's gzip responses with a 512 KiB wire cap and 1 MiB decoded cap, recording encoding and both byte counts. Each HTTP cycle performs four asset reads, two summary reads, one MCP `tools/list`, then one MCP `echo` and one invalid-arguments check. Work is sequential within each client; slow responses reduce that client's actual rate instead of creating a backlog. Summary responses remain raw CBOR at the controller and are recorded as bounded byte counts and SHA-256 digests. Static asset hashes are checked per path for consistency through the stage. WS uses a fresh random 16-byte client key and masked RFC 6455 binary frames carrying the demo's CBOR `echo.request`; it counts valid unsolicited summary/hello messages and handles ping/pong/close controls while waiting for the matching echo ID and exact text. SSE requires `text/event-stream`, parses bounded lines and data events, counts progress by client, and resumes with `Last-Event-ID` after reconnect.

`-asset-mode reload` is the default and retains the page-reload stress mix above. `-asset-mode cached` models an already-loaded browser UI: it discovers and serially fetches each asset once before protocol workers start, records each asset hash and the prefetch duration, then sends summary reads in the four HTTP asset-cycle slots. HTTP cadence, MCP, WS, SSE, socket caps and timeout thresholds remain the same. Every JSONL record includes `asset_mode`; use cached mode for longer representative soaks and reload mode for repeated page-load stress.

MCP uses the source-defined `/mcp` endpoint, protocol `2026-07-28`, headers mirrored from the JSON-RPC request, `Origin` set to the controller origin, and the Showcase fixture credentials `demo/showcase-only`. The endpoint must be a demo that exposes these tools and fixture. The invalid call supplies an empty argument object to `echo` and expects JSON-RPC `-32602`; it does not call experiment/control tools.

## Output and resume

The JSONL file is opened in append mode and synced after each bounded record. Every second it records total/per-client counts, HTTP status counts, the configured HTTP socket cap, selected asset paths, recent p50/p95/max HTTP and WS echo latency, timeouts/errors, body sizes/encodings/hashes, asset-integrity failures, WS summary/hello and ping counts, SSE event counts and last event ID, and the device `boot_id` when available from CBOR `/api/v2/info`. A changed known boot ID cancels the load and is recorded as the stop cause; the final recovery probe still runs. A final record follows cancellation only after all managed WS/SSE sockets are closed and worker goroutines have drained; it includes `/health` and `/api/v2/info` recovery results. `Ctrl+C` and the duration deadline take the same cleanup path. Durations are bounded to 24 hours, responses to 64 KiB, SSE lines to 4 KiB, SSE events and JSONL records to 16 KiB; asset downloads allow 512 KiB on wire and 1 MiB after decompression.

If a process or host stops unexpectedly, rerun with the same `-out` path and `-base`. The next run appends a new run ID and resumes SSE from the last complete JSONL sample's event ID. It rejects an output file whose last complete record belongs to another base origin or is malformed. Keep output files in `tools/load/`; its local `.gitignore` excludes `*.jsonl`. Without `-out`, the default is `run.jsonl` relative to `tools/load/`.

The harness intentionally does not start a local server, discover/close browser connections, send commands that alter device/application state, or configure/flash hardware. Review the run's `stage` records and per-second samples together; a recovery probe is a separate end-of-stage observation, not proof of device-side memory return or an overnight pass.

## Local validation

From the repository root, run `go -C tools/load test ./...` and `go -C tools/load vet ./...`. The tests exercise CBOR boot identity, float/null/bool decoding and bounds, the optional local C6 summary captures, option validation, append/resume origin checks, stop-file detection, bounded error-reason counts, bounded SSE parsing, 16-byte WebSocket key and accept validation, hashed asset discovery, gzip limits, and a local in-memory RFC 6455 binary CBOR echo with ping/pong and an unsolicited summary. They do not open a port or contact a device.

## WebSocket close probe

`-profile ws-close -base http://<verified-device-ip> -out <fresh-report.json>`
opens ten sequential owned connections: five client Close 1000 exchanges and
five 4097-byte messages against the fixture's 4096-byte limit (Close 1009).
It records time from the client write to the complete server Close frame and
TCP EOF separately, with the unchanged four-second per-connection deadline.
Capacity, REST automation and mixed overload are separate profiles. Every owned
socket is closed; final recovery and boot identity are checked. This profile
does not treat a successful server send or an observed TCP reset as a Close pass.

## Admission/QoS acceptance

`-profile qos -qos-key <configured-primary-id> -duration 60s -out <fresh.jsonl>`
separates the reserved REST automation (2 requests/s), an anonymous keeper
(1/s), and two 50ms mixed workers which exceed guest rate/identity capacity.
At most four requests are outstanding. It checks complete 503/429 responses,
Retry-After, protected 200 replies, boot continuity, inline sampled internal
free memory, and a one-second refusal bound. No retry changes acceptance.
The key is supplied by the caller and is not logged. The demo fixture's
deliberately public identity is not an authentication credential.

`-profile qos-streams -qos-key <configured-primary-id> -out <fresh.jsonl>`
holds one guest WS/SSE, attempts ten extra connections to each, checks full
503 replies, admitted WS pong/SSE progress and protected REST. Its 350ms cycle
keeps this capacity test below the guest rate ceiling; rate limiting is tested
separately above. Overall deadline is60s. This is fixture-specific acceptance,
not automatic discovery of every library configuration. Normal rest/ui profiles
and raw ingress/SYN saturation remain separate.

## Memory sampler (read-only observer)

`sample-memory.mjs` is a separate low-rate read-only observer. It uses `curl.exe` with `--noproxy *`, source `192.0.2.3`, four-second request deadlines, and a 64 KiB response cap. It reads `/api/v2/info` once for firmware build identity and boot ID, including the validated ELF SHA-256, JSON and HTTP backends, and MCP workspace/input/output limits. Each interval it records every memory region (including PSRAM), CPU, and up to eight task names/stack watermarks. Task data is accepted only when its device source and boot ID match the summary from the same sample. It does not record Wi-Fi identifiers, credentials, or full CBOR payloads.

```powershell
node tools/load/sample-memory.mjs --base http://<verified-device-ip> --out <absolute-ignored-path.jsonl> --stop <absolute-stop-sentinel> --duration-seconds 28800 --interval-seconds 60
```

The sampler appends JSONL records and polls the stop sentinel every second between samples. It also checks the sentinel between summary and task reads, records boot changes while continuing observation, and caps duration at eight hours. It makes no mutation requests and opens no listener.

Source addresses are never inferred from the author's machine. Supply -local-address; the memory sampler uses NAWSERVER_SOURCE_ADDRESS. Device origins and credentials stay in ignored local configuration.

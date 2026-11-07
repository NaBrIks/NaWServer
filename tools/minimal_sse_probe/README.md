# SSE probe for examples/minimal

This is a small Go standard-library probe for the current **examples/minimal**
routes on the identified experimental C3. Both native HTTPD and shared TCP are
supported. It does not use the different `tools/sse/integration_device` contract.
It never flashes, opens serial, reads Wi-Fi credentials or changes NVS.

Run after the ordinary HTTP/WS and browser checks, with an independent serial
observer recording panic/assert/watchdog failures. Start with no unrelated SSE
clients: the probe checks initial active/queued counters and rejects interference.
The only Basic credentials are the public fixture `test:test-only`.

```powershell
go -C tools/minimal_sse_probe test ./...
go -C tools/minimal_sse_probe run -buildvcs=false . --url http://CURRENT-C3-IP --backend tcp --out <repo>/local/UNIQUE-minimal-sse.json
```

Use `--backend httpd` for the native profile. This checks the generated frontend
transport label; firmware hash/SDK/device identity still belong to the flash and
serial evidence. Every result path must be unique to the SDK/backend/run.

The default stage checks public and authenticated private admission, missing/wrong
Basic rejection before any SSE bytes, connected events for absent/empty/nonempty
Last-Event-ID, exact UTF-8/NUL/multiline/trailing-LF publication with event/id/retry,
and close with observed counters. Cases use one stream at a time to avoid treating
a partially accepted broadcast as delivery to every client. HTTP `200 queued` and
`200 closing` are acknowledgements, never completion: the event must be received,
and active/queued must return to zero with the exact begun/completed delta.
The close request may be repeated while waiting for this bounded drain.

Optional **last** stage:

```powershell
go -C tools/minimal_sse_probe run -buildvcs=false . --url http://CURRENT-C3-IP --backend tcp --stop --out <repo>/local/UNIQUE-minimal-sse-stop.json
```

It opens the requested SSE clients (default four) and one WS witness, queues a
bounded publication, then requests `/events/stop`. This stage requires
`clients + 2` application connections including one HTTP control request, **plus
all existing device connections**. It observes SSE closure,
exact completed counters, closure of the old WS witness, health recovery, and a
new WS handshake/echo. This is an observable restart check without a server
generation API; SSE stop alone would not close the WS witness. It accepts a reset
or an unterminated HTTP chunk body as a closed stream, and does not promise that
queued events were delivered during shutdown. **SSE remains stopped afterward**;
run this last and reset through the coordinated owner before another SSE run.

Diagnostic capacity control, mutually exclusive with `--stop`:

```powershell
go -C tools/minimal_sse_probe run -buildvcs=false . --url http://CURRENT-C3-IP --backend httpd --admission-only --out <repo>/local/UNIQUE-minimal-sse-admission.json
```

This first runs the same three delivery cases, then sequentially opens the requested SSE
streams through the **same** raw open method/deadlines as `--stop`, with no WS
witness. It checks exactly that many simultaneous active admissions and counters,
then requests close and observes the same number of completions and zero active/queued bytes. It does
not request stop, check restart, or change the default `--stop` fixture of four SSE
plus WS. The JSON `profile` and `stage` label this diagnostic explicitly;
`admission_active_observed`, `admission_sessions_completed` and
`closed_sessions_observed` separate these admissions from the three preceding
delivery cases. No admission success is inferred from an HTTP acknowledgement.

`--clients N` accepts **1..4**, defaults to **4**, and applies only to the optional
stop/admission stage. The arrays and protocol bounds remain fixed at four. Default
four-client stop needs an isolated six-connection budget; four-client admission
needs five. Preserve existing owner pages and processes: account for their sockets
rather than closing them or raising firmware limits. For example, with three
existing Chrome connections and a native seven-connection limit:

```powershell
go -C tools/minimal_sse_probe run -buildvcs=false . --url http://CURRENT-C3-IP --backend httpd --stop --clients 2 --out <repo>/local/UNIQUE-minimal-sse-stop-clients2.json
```

Three existing + two SSE + one WS witness + one HTTP control = seven. This verifies
**two** simultaneous SSE clients, not four. JSON `requested_clients` and observed
counts preserve that distinction. Each admitted stream must return its headers and
connected event within the original deadlines; the exact active-N snapshot must
precede close/stop. Begun/completed counters collected after a failed probe exits
can include late admissions after earlier sockets close, and do not prove that N
clients were admitted before the deadline. A failed probe is never upgraded to PASS
using those later counters.

The stop stage also checks HTTP health immediately after the old WS handshake,
before opening any SSE clients. Its failure prefix is `HTTP health with idle old
WS`; this distinguishes an idle-WS interaction from subsequent admission pressure.
With `--out`, failures also write `status: FAIL`, the stage/error and only counters
already observed during the run. `before_observed`/`after_observed` distinguish
real snapshots from unused zero fields; `last_observed` is the latest successful
stats response, not a post-cleanup query. Saving a failure report performs no extra
network operation and cannot turn later admissions into a successful run.

Bounds: whole run 90 seconds; socket/HTTP operations four seconds; close/drain
phases seven seconds plus bounded individual calls; one HTTP connection at a time;
SSE response head 8192 bytes/32 fields and 1024 bytes per line; ordinary HTTP heads
8192 bytes; HTTP response body 4096 bytes; SSE event 4096 bytes/64 lines and 1024
bytes per line; closing stream tail 32768 bytes. The WS witness only handles small
complete text/tick frames, at most four frames for an echo and sixteen before
closure. This is neither a universal SSE/WS client nor a load, slow-reader, fuzz,
soak, heap-leak or hard realtime shutdown test. It does not detect device panics
without the separate serial observer.

Unit tests use only strings and an in-memory HTTP transport, with no listening
sockets or device requests. They cover bounded parsing, split/coalesced HTTP heads,
empty/inherited IDs, binary data, malformed fixtures, missing counters, and the
distinction between acknowledgement and observed completion. Passing them is not
hardware evidence.

Failures identify the stage and operation, for example `case2 private-last-id
open: SSE /events/private response headers` or `stop stage: SSE4 connected event`.
These labels preserve the original socket/HTTP error without printing event
payloads or Basic credentials. A timeout is a failure; delivery/drain predicates
are not relaxed by the diagnostics.

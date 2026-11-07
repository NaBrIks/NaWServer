# Short transmission scenarios

Run from this repository using its installed Node.js 24+ and `cborg` dependency:

```powershell
node tools/load/transmission-scenarios.mjs --base http://DEVICE --scenario stress --seconds 30 --label tx8
```

`--base` is required. It accepts a plain HTTP origin without credentials or a
path. `--seconds` is an integer from 1 to 120 (default 30).
Labels contain only ASCII letters/digits/dots/underscores/hyphens, maximum 64
characters. Reports are always saved beneath ignored `local/transmission-bench`.
No configuration, reboot, filesystem or firmware operation is performed.

| Scenario | Controlled traffic during the measurement window |
| --- | --- |
| idle | No persistent observers or load requests |
| rare_api | One health request halfway through the window |
| ws | One WS summary subscription |
| sse | One SSE subscription |
| both | One WS and one SSE subscription |
| half | Half the advertised WS capacity, SSE, summary HTTP once per second, echo every 500 ms |
| stress | Advertised available WS capacity, SSE, two continuous JS asset lanes, echo every 200 ms |

`--echo-hz=0..20` overrides the per-client echo rate (default half: 2 Hz, stress:
5 Hz, other scenarios: 0). Use `--echo-hz=0` for pure egress stress without echo
callbacks. A positive rate requires a scenario with WS clients. Both `--name value`
and `--name=value` forms are accepted for valued options.

WS capacity comes from `/api/v2/info.limits.max_ws_clients`. Existing WS clients
reject the run by default. `--allow-external` explicitly allows using remaining
slots and marks the report `contaminated`; it does not make the result a clean
acceptance measurement. Quiet scenarios additionally require zero WS clients at
the final boundary and reject HTTP counter growth above eight requests. This
retains a small allowance for the harness's boundary snapshots; report fields
include the observed counter delta and the harness's own request count.

Streams are established before a two-second settling interval and the initial
summary/task snapshot. CPU is computed from cumulative task runtime deltas at
the boundaries, normalized by matched-task runtime sum. It is not the device's
last instantaneous CPU sample. Missing/new tasks are recorded as a limitation.

The final CPU snapshot is taken after load producers stop and before a bounded
three-second echo drain. Every attempted echo is counted; missing/duplicate/wrong
replies fail the run. Reports retain per-client summary count/gaps, sent/replied/
pending echoes and RTT p50/p95/max, SSE counts, HTTP sizes/durations/failures,
boundary heap/task/TX data and periodic WS samples for correlation. Throughput
uses the actual load duration, including completion of in-flight HTTP requests.

Stress discovers a same-origin JS asset from `/` and obtains a reference body
length and SHA-256 before measurement. Each downloaded body must match both. Node
fetch transparently decompresses gzip: `received`/`bodyBytes` describe decoded
bytes; `contentLength` and `contentEncoding` preserve the wire header values.
The tool does not confuse compressed Content-Length with decoded size.

Buffers and retained records have explicit bounds. Truncated record collections
are indicated by `droppedRecords`; totals continue counting all attempts.
Startup failures, unexpected stream closure, HTTP errors/truncation, missing
echoes, reset and increased TX failure counters produce exit code 1; invalid
arguments produce 2. `--help` contacts no device.

Use equal firmware/profile, clients, duration and Wi-Fi conditions for comparison.
Lifetime heap minima reset only after a device restart, which this tool does not
perform. This short scenario tool does not establish long-duration reliability.

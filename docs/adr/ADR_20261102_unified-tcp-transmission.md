# Shared bounded TCP transmission jobs

Date: 2026-11-02. Status: implemented on the experimental unified TX branch.

The owner requested one bounded collection of transmission jobs for HTTP,
WebSocket and SSE, common payload memory, producer leases, priorities and explicit
expiry/replacement. This supersedes the earlier separation of protocol TX budgets
only when `TcpOptions::transmission.enabled` selects this shared path.

## Decision

`QueuedTransport` wraps the application's concrete TCP transport inside one
`TcpHttpBackend` runtime. Protocol readers and serialized application callbacks
retain their existing execution model. One Transmission worker performs native
writes for all wrapped sockets. Every native handle receives a monotonically
increasing generation on registration; queued jobs retain that generation.

The queue is one bounded job table, not a strict global FIFO. Jobs of a given
connection preserve reservation order, including uncommitted producer leases.
Other connections may progress while that producer fills its block. The worker
limits native write size, retains partial progress, skips temporarily blocked
sockets and uses a periodic fairness turn in addition to priority/age selection.
Network calls run outside the job-table lock.

Public `WriteBuffer` is a move-only producer lease: acquire, fill directly,
commit actual length or cancel through RAII. Uncommitted memory is never read or
forcibly reclaimed. Static caller-owned payload, initially allocated fixed
blocks, bounded dynamic blocks, or a mixture are supported. Dynamic blocks return
immediately after completion/cancellation. Fixed job metadata is embedded in the
transport; maximum configured capacity is 32 jobs, independently of payload size.

Normal work cannot consume protected important job slots or byte capacity.
The existing primary API identity receives important per-connection scheduling
policy after admission; priority never bypasses authentication or admission.
Explicit per-producer policy cannot lower that protected connection priority.

The default block capacity is configurable with
`NAWSERVER_TCP_TRANSMISSION_BLOCK_BYTES` (four 1460-byte MSS units by default).
Profiles may set byte capacity from their actual SDK MSS. Block capacity and
per-dispatch I/O size are separate options. Neither guarantees one complete
socket write. Successful enqueue means queue ownership, not TCP ACK or delivery
to the peer application.

## Protocol integration

HTTP `SessionIo` writes route through the wrapper. `ResponseSource` fills queue
leases directly, and the sequential Response writer waits only within configured
exchange/stall bounds. The synchronous handler retains its borrowed source until
return. Unknown-length chunked streaming and deferred borrowed source lifetimes
are not implied by this known-length API.

Shared-mode WS producers encode a complete frame directly in a shared lease.
Legacy WS TX payload pools allocate no payload in this mode. Control and Close
frames are also committed as indivisible jobs, so application producers cannot
interleave bytes between their header and payload. An endpoint's maximum frame
must fit a configured shared block. The old TCP path and other backends retain
their existing queues when shared transmission is disabled.

SSE uses the same shared transport and optional direct Stream acquisition. Its
direct adapter retains protocol/session metadata and heartbeat/stop rules while
frames move directly into common jobs, rather than a separate per-client
FrameSet payload backlog. Native HTTPD remains independently implemented.

Explicit `WritePolicy` contains priority, absolute expiry, permission to discard
an unstarted complete unit, and an optional replacement key. Committing a new
matching unit may replace only a Ready job with zero native progress. Filling,
Writing or partly transmitted jobs cannot be replaced. Ordinary HTTP chunks,
command replies and default telemetry have no implicit discard/coalescing.
Unsupported backend policy calls reject the request rather than ignore policy.

## Lifecycle and observations

Closing a socket stops new producers, drains queued writes within a finite bound
and then delegates one native close. A Filling or in-flight Writing job prevents
unsafe close/reclamation; the retained outcome is retried through existing
lifecycle rules. Runtime destruction follows joined protocol owners and the
Transmission owner. External producer leases must end before destruction.

`TcpHttpBackend::transmissionStats()` exposes configuration, filling/queued/native
active jobs, retained/active/peak payload bytes, current remaining queued bytes,
accepted/native-sent bytes, completions, rejections, failures, explicit discards
and native WouldBlock counts. These payload figures exclude embedded metadata,
worker stacks, allocator overhead and TCP stack buffers. API sampling is supported
while the runtime is alive; application lifecycle calls remain owner-serialized.

Meaningful regression checks include direct static payload, mixed/dynamic byte
limits and release, important byte reserve, per-handle inherited priority,
uncommitted producer isolation, partial native writes, blocked-connection
isolation, explicit expiry/replacement and generation-safe close; known-length
5/160 KiB Response source/writer/direct-lease output; and simultaneous 160 KiB HTTP
stream, two WS producers/control frames and SSE under a blocked HTTP callback.
Actual acceptance results and device evidence belong in the task verification
report; this decision does not claim a universal hard latency bound.

# Current component architecture

Demo RAM visualization is application-owned: optional memory_usage in summary
combines actual linker data boundaries and SDK heap samples. The ESP buffer
allocator exposes lightweight live payload counters by actual internal/PSRAM
address; no global tracing or demo knowledge is added to the public core.
Untracked allocations/allocator overhead remain explicitly other dynamic memory.
See docs/readme_memory_distribution_ru.md for scope, fallback and verification.

## Managed buffers and persistent HTTP (2026-10-27)

The portable MemoryManager holds only its configured initial blocks, allocates
additional actual-size blocks within slot/byte/free-memory ceilings and frees
dynamic blocks immediately on return. MemoryBlock is a move-only lease with an
explicit manager lifetime. TCP HTTP metadata/responses, lazy request bodies,
per-upgrade WS arenas and lazy SSE frame sets use this mechanism; bounded ingress
and descriptor tables remain allocated at startup. Physical primary HTTP reserve
is opt-in and distinct from admission quotas. Central size-specific readiness and
block reasons permit early rejection; existing fixed blocks remain reusable under
heap pressure. Platform probes/guarded allocation stay behind BufferAllocator;
Windows/Linux assume no ESP heap or global system-free-memory threshold.
Response.sendOwned transfers a lease without a second TCP payload copy, while
sendImmutable and normal lifetime-safe copies retain their existing contracts.
Optional sequential HTTP/1.1 keep-alive retains only socket/idle metadata between
requests and rechecks admission each time. Idle/request ceilings and explicit
Connection: close remain bounded. This replaces all-job/all-arena startup
reservation; it does not introduce pipelining, concurrent callbacks or universal
SDK allocation control. The compiled scheduler can be used without Admission.
See docs/readme_buffer_memory_ru.md and
docs/readme_20261027_memory_manager_keepalive_ru.md for configuration and evidence.

## Optional admission and QoS (2026-10-27)

Admission<N> is an allocation-free independent policy with static client and
permit tables. A configured primary key has its own quota, and admitted shared
identities retain individual quotas across requests until idle expiry. Keys
classify resource ownership; application authentication remains separate.
TCP optionally applies this policy before bodies, with bounded ingress,
per-identity HTTP jobs and private WS arenas. Small 503/429 replies and bounded
I/O steps preserve serial, non-reentrant callbacks. Normal Response sends copy
bounded data before return; sendImmutable explicitly borrows immutable body
storage valid through server stop. Middleware can reuse the same policy without
charging an already admitted TCP request twice; alone it is a later gate.
An optional trusted-peer callback reserves one ingress lane for an
application-selected IPv4 without granting the primary key quota or bypassing
authentication. All ingress/native socket resources remain finite: raw TCP
saturation differs from completed HTTP rejection. The compact TCP adapter has
one WS per identity; applications coordinate total WS/SSE/socket ceilings.
Compile-out options are CONFIG_NAWSERVER_TCP_ADMISSION and the host build option.
Demos only supply test configurations. See docs/readme_admission_ru.md and
docs/readme_20261027_admission_qos_ru.md for measured cost and acceptance.

## Buffer memory and resource ceilings (2026-10-27)

Public TCP HTTP/WS and SSE frame storage have separate per-instance allocators
and hard requested-byte budgets. The generic core knows no LCD, demo or board
policy; ESP-IDF offers strict internal/PSRAM and explicit preference/fallback
policies through its helper and Kconfig defaults. JSON/MCP retains caller-owned
workspace. Internal task/synchronization and OS/SDK storage remain separate.
Capability malloc alone guarantees only four-byte alignment; the ESP buffer
allocator and caller-owned JSON storage explicitly satisfy max_align_t.
Admission, message and queue limits reject excess work before payload allocation.
Normal REST automation (1/2/3 sequential clients), HTTP UI visits and overload
are different test profiles. Follow-up TCP diagnosis enabled IDF TCP_NODELAY and
public bounded HTTP/WS close grace/discard limits (250 ms / 8192 bytes by default).
Packet capture exposed stale TCP state on reused source ports and WS resets with
unread input; the short ordinary matrix now passes, while reload overload still
has timeouts. Neither increased free RAM nor these short runs prove soak acceptance.
See docs/readme_buffer_memory_ru.md, docs/readme_20261027_memory_limits_tests_ru.md
and docs/readme_20261027_tcp_rest_ws_close_ru.md.

## Shared TCP engine (2026-10-25, P6 code/builds; C3 acceptance incomplete)

The owner selected one HTTP/WS wire engine over Winsock, Linux sockets and IDF
lwIP BSD sockets, retaining EspIdfHttpBackend as an independently selectable
HTTPD path. Keep HttpBackend/HttpExchange and the common dispatcher; reuse the
existing SSE encoder/Sessions/bridge. This supersedes the IDF-only platform
restriction, not the application boundary or the prohibition on a universal HAL.
The shared TCP HTTP/WS/SSE path and bounded storage are implemented on Windows,
isolated Linux and IDF through concrete tcp::IdfTransport and private FreeRTOS
worker glue. Native HTTPD and the older Windows/memory paths are retained.
P6 code and SDK builds are implemented, but the stage remains open: C3 functional
checks are partial and browser failures are under investigation. MCU load/soak
acceptance remains pending in P7. Stage evidence belongs in docs/readme_tcp_engine_verification_ru.md.

Our code must avoid unnecessary copies and allocations: use stable bounded storage
and views, direct synchronous output and explicit owned queued data. Every substantial
copy/allocation needs a lifetime, capacity and reason. KiB-scale buffers, task stacks,
queues, retained metadata and Flash dependencies must be measured. Static storage
is not automatically smaller; runtime queue limits are not reserved storage size.
Keep safe ownership copies; do not claim end-to-end zero-copy or zero-heap.

The native HTTPD/TCP-only choice and independent WS/SSE flags are implemented in
Kconfig and component CMake. Standalone TCP-only graphs exclude HTTPD/esp-tls;
other application components may add those dependencies independently. Sixteen C3
SDK build profiles passed at the checkpoint before safer-close:
IDF 5.4.0/6.0.3 × HTTPD/TCP × HTTP/WS/SSE/WS+SSE.
Native rows were refreshed after the SDK6 post-handshake fix; pre-fix artifacts
remain separate. The current safer-close MVP was rebuilt in two full TCP profiles,
IDF 5.4.0 and 6.0.3 with HTTP+WS+SSE: 2/2 passed. The entire 16-profile matrix was
not repeated after this change. Other minors/SoCs are unverified; C++17 remains
the language baseline. The owner's current priority is a working C3 MVP, with
Windows/Linux restricted to fast, focused development checks.

Current native C3 functional observations include on-device response 89 / loopback 63 /
lifecycle 30 and bounded LAN HTTP/WS/SSE checks. Native IDF5 and IDF6 after the post-hook
fix passed Go HTTP/WS, SSE admission with three clients and SSE stop with two clients
and a WS witness. This is not a complete
browser gate: browser failures/connection stalls remain open and are not declared
fixed by one successful repeat. P6 remains open; exact profiles/results belong in
the stage report, not a blanket claim of support for every IDF5/6 release.
Current safer-close TCP/C3 builds on both tested SDKs passed on-device 89/63/30,
LAN HTTP/WS, the 78-check response probe with two clients, SSE admission with three
clients and SSE stop with two clients plus a WS witness. SDK5's normal browser run
kept one WS and two SSE streams for 20 iterations: each SSE stream received all 20
events, 40 WS echoes matched, and exactly two SSE streams drained. SDK6's equivalent
normal steady run also passed: 20 health checks, 20 text and 20 binary WS echoes,
and 20 events delivered to each SSE stream. SSE begun/completed reached exactly
2/2 with active/queued both zero after one cleanup attempt; EventSource remains
running. An earlier repeated full-UI coexistence run hit a WS timeout and remains
recorded separately. This establishes the scoped C3 MVP, not four-client, full P6,
full-UI regression or soak acceptance.
Acceptance includes deterministic partial-I/O tests, real browsers, MCU-appropriate
2–4-client mixed load, bounded overload/recovery and soak with memory/latency evidence.
Production-adjacent Linux testing uses a separate restricted process; destructive
stress/fuzz/OOM scenarios belong on an isolated host.

Read docs/adr/ADR_20261024_nawserver_shared-tcp-http-engine.md,
docs/reviews/2026-10-24_shared-tcp-http-engine.md,
docs/plans/PLAN_20261024_nawserver_tcp-engine-contracts.md and
docs/plans/PLAN_20261024_nawserver_tcp-engine-implementation.md before implementation.
The research records a historical host result of 17/18 with a subsequent isolated SSE
pass, not a clean acceptance. Proposed runtime details and numeric budgets are
engineering recommendations; completed support must follow measured evidence.

## Shared HTTP path and dated checkpoints

The shared HTTP path uses the explicit `tcp::Transport` contract, concrete
Winsock/Linux/lwIP adapters and fixed generation/ownership slots, with scripted
transport tests. The allocation-free bounded parser/writer now feed
the same HTTP dispatcher through TcpHttpBackend and a serialized worker. P4 uses one
reusable HTTP body workspace and a validated borrowed preparedBody view; other
backends retain the owned RequestBody fallback. OS worker glue is private. Shared
WS handshake/frame codec and SSE encoder/Sessions/bridge preserve the original
handlers and synchronous HTTP pipeline. The dated P2 result was 28/28 host tests
and 12/12 browser fetch plus repeat; current P6 evidence is recorded separately in
docs/readme_tcp_engine_verification_ru.md.
C3 compiler-only P4 startup allocation requests are 13395 B for HTTP-only or
29879 B with four WS table entries and one shared 4x4096 B TX pool, including
array cookies, before allocator/worker/map overhead. Queued and active WS frames
own move-only pool leases. Busy and GlobalFull retain the connection; individual
QueueFull requests close1013 through the I/O owner, preserving any partial frame.
HTTP-only does not allocate pool backing. These sizes and compiler frames are not SDK runtime
heap/stack measurements and do not replace the separately recorded C3 functional runs.
Usage, backend/feature selection and explicit startup/stop: docs/readme_tcp_http_ru.md.
SDK graph/link validation is separate from the host source list. Ambiguous retained
close errors pin cleanup rather than
blindly retrying a potentially reused descriptor.

P5 passed native isolated Linux GCC/Clang and ASan/UBSan41/41, Windows44/44,
a separate TSan concurrency/lifecycle corpus and two hour-long wire fuzz jobs.
That historical P5 revision used one shutdownWrite followed by bounded TCP drain.
Current HTTP/WS cleanup discards up to 4096 B/50 ms before the owner's one close,
without a separate half-close. HTTP reuses its 256-byte scratch; WS reuses Rx after
the complete Close frame, with no further callbacks. SSE directly uses registry
close with its existing retained-close/retry contract and no new RX drain.
Transport::shutdownWrite remains a real half-close API for explicit callers;
cleanup does not call it. Safer-close adds no buffers, retained storage or allocations.
The deadline bounds I/O, not hard real-time reclamation while the owner is busy.
C3 connection sizeof1008 B is compiler-only, not hardware heap/high-water.
The SseTest diagnostic app retains owned metadata on initial Busy and publishes
from explicit owner poll; core Busy/queue policy is unchanged. Its C3 object+592 B
and poll own frame1024 B belong in the sample budget, not every application.
Current scope, limitations and reproducible evidence remain in the stage report.

The owner authorized a narrow HTTP backend boundary for in-memory tests, Windows
and IDF. HttpBackend/HttpExchange/HttpRequestHandler live in http_backend.h;
HttpServer in http_server.h borrows its backend and a static route table.
Shared dispatchHttp preserves body -> query/form -> handlers -> pipeline
-> endpoint -> Response. Request and Route include no SDK headers;
HttpMethod/HttpError belong to the component. ResponseTransport remains the shared
synchronous send boundary. This is not a universal HAL/RTOS/network abstraction.

The host library builds from host/CMakeLists.txt without SDK stubs. MemoryHttpBackend
submits decoded requests to the same dispatcher and models read/send failures.
WindowsHttpBackend uses Winsock, loopback, serialized callbacks, bounded HTTP/1.0/1.1,
Content-Length and Connection: close. Windows and memory also support the shared
WebSocket endpoint and SSE; host TLS is not implemented.
frontend/http-test/ uses separate HTML/CSS/JS to test the backend with real fetch,
without copying the existing demo UI.

Native Server remains an HTTPD facade, owns EspIdfHttpBackend and retains route validation/dispatch.
backends/esp_idf.h and src/backends/esp_idf.cpp own HTTPD configuration, startup,
native WS registration, callback contexts and stop. Ordinary HTTP uses the shared
dispatcher and EspIdfHttpExchange. SDK access lives in backends/esp_idf_request.h;
SSE uses this extension. WS uses the platform-neutral WebSocketExchange extension.
HttpServer can compose HTTP and WS routes with the native IDF, memory, Windows
and shared TCP backends within each backend's implemented platform scope.
Failed startup plus failed rollback returns CleanupRequired through the common
API: HttpServer is not running, but retains cleanupRequired until successful end.
Never release handles/contexts/dependencies after an unsuccessful stop. Native
Server.running still reflects handle ownership, including failed startup cleanup.
Native parser/socket-wait settings remain SDK-owned; unsupported non-default host
settings are rejected. This extraction adds no new transport deadline policy.
Host lifecycle harnesses compile canonical backend/facade sources against controlled
SDK stubs with WS=0/1 and the SDK6 post-hook model. They do not verify real SDK
tasks or device behavior. Historical HTTPD revision from2026-10-24:
89 response checks, 63 loopback checks and 27 lifecycle checks passed; LAN smoke
and 198 HTTP response checks passed. Shared mini UI passed 14 browser checks on C3.
Builds run in the background. No other MCU/hardware result is implied.
New host/browser results are in docs/readme_http_api_verification_ru.md;
hardware results below remain historical. Small utilities/decoders/CBOR are unchanged
by this stage.

## Source layout (reviewed 2026-10-25)

Public API headers live in include/NaWServer and use <NaWServer/...> includes:
server.h, request.h, response.h, websocket.h and sse.h are the main entry points.
Implementations are grouped under src/backends, src/http, src/auth, src/routing, src/websocket
and src/sse. CMake exports only include; src is a private include directory.
SSE implementation headers required by its statically owned concrete class live
in include/NaWServer/detail/sse. They are implementation details, not application API.
Host harnesses may explicitly include detail headers for white-box verification.
Historical reports and hashes retain the paths used for the measured revision.

Private shared protocol helpers live in `src/detail/ascii.h` (locale-independent
ASCII comparison) and `src/http/header_validation.h` (HTTP field-name/value syntax).
Response and SSE use the same syntax checks but retain their own reserved-header,
duplicate, lifetime and capacity policies. Basic auth scheme and form media-type
comparison reuse the ASCII helper. These headers are not application API.

The MCU application selects native ESP-IDF HTTPD or shared TCP, using C++17.
Application code owns network initialization, storage mounts, product data, handlers,
providers, middleware, chains and route tables. HttpServer borrows its explicit
backend; the native Server facade owns an EspIdfHttpBackend and its HTTPD instance.
Constructors bind dependencies and remain inert; `begin()` starts runtime resources.
Initialize esp_netif before starting IdfTransport. Stop producers and drain EventSource
before server.end(), then stop the transport before destroying borrowed dependencies.
Worker glue is private, not a public task/clock HAL. Never stop from an HTTP callback.
Server copying is forbidden.

## HTTP routing and middleware

- `pipeline.h/.cpp`: transport-independent synchronous executor, request-local Context,
  Principal, fixed permission masks and route AccessPolicy. Typed `Chain<T...>`,
  `makeChain(instanceRefs...)` and `FixedChain<N>` own fixed arrays of non-owning
  MiddlewareRef and use one executor. Execution itself performs no heap allocations.
- `handler_middleware.h/.cpp`: AuthenticatorRef provider contract, AuthMiddleware,
  AccessMiddleware and one-user FixedBasicAuthenticator with borrowed code credentials.
  Providers expose authenticate(request, principal) and challenge() (borrowed
  WWW-Authenticate value or nullptr). Route does not assume Basic; Context resets
  challenge metadata with Principal on every run. No NVS, filesystem user store or
  dynamic role catalog is implemented.
- `route.h/.cpp`: exact HTTP method/path, borrowed endpoint/pipeline and copied policy.
  Permission checks fail closed before the terminal endpoint even without an explicit
  AccessMiddleware. Public routes may use an empty chain. Identity resets per run.
- `Next` is a non-copyable stack continuation valid only during the middleware call,
  at most once. It is not a task/coroutine or deferred HTTP API. A repeated call does
  not run downstream again and produces a sticky InvalidNext result.
- Route maps unauthorized/forbidden to 401/403, missing response/error to 500.
  Response records send state/failures and prevents a second wrapper send.
  Errors after a response close the connection through the dispatcher; they cannot
  retract bytes already sent. Native HTTPD sends bypass wrapper accounting.
- `Server` accepts a borrowed static HandlerBase pointer table, or separate
  pre-start addHandler registration. Mixing them is rejected. Runtime routes must not
  be mutated. HTTPD URI/context startup allocations still exist for the native backend.
- `HandlerBase` remains the endpoint/transport interface. Legacy decorator forwarding,
  owning WebServerContainer and unused body/upload/trivial callbacks were removed.

`src/http/handler_static.*` retains flat borrowed memory/text/binary/gzip handlers. They do
not require middleware. MIME is explicit (text defaults to text/html, binary to
application/octet-stream); file/source serving remains a follow-up;
filesystem mounts belong to the application. Query/form fields are prepared before
handler dispatch by a length-delimited parser; malformed escapes reject the complete
request with 400. Two compile-time percent backends share one contract: Lookup
(default) and std::from_chars. Field maps own decoded strings including embedded NUL;
this is not a zero-heap library. Query/form keys are exact/case-sensitive, first
duplicates win, form fields override query fields, and partial results never escape.

RequestBody either owns std::vector<uint8_t> or borrows an immutable ByteView until
the callback returns. Shared TCP reads into one stable reusable body workspace:
it copies the prefetched prefix once, then receives the remainder at final offsets.
The dispatcher validates preparedBody length and pointer before borrowing it;
malformed capabilities fail closed. Other backends retain the owned setBody()/readBody()
fallback. body() returns a borrowed immutable view in either case.
copyBodyTo supports pointer/capacity, C array, std::array and std::vector. Fixed
destinations fail without writes on insufficient capacity; vector may allocate and
reuses capacity. No payload codec belongs to this API. Raw/view lifetime ends with
the callback; copies belong to the application. query/form/parameter return borrowed string_view fields until request destruction.
Query/form preparation borrows the raw input views and materializes
decoded fields; only application/x-www-form-urlencoded bodies are parsed (case
insensitive media type, optional parameters). Other bodies remain raw, even if they
contain '%' or NUL. path()/rawQuery() are raw URI views; query/form/parameter expose decoded fields. See docs/readme_url_decode_integration_ru.md. HTTP body is still fully read before handler dispatch, bounded to 4096 bytes. Auth
before body consumption and streaming upload are not implemented by this stage.
Basic compares complete credentials; empty username/password denies access. HTTP
Basic does not supply TLS. Tests use public fixture values, never device secrets.

## WebSocket and execution

Native WS is separate from HTTP Route. Public WS works in the baseline; authenticated
WS remains unsupported because HTTPD can upgrade before the application callback.
There is no legacy auth wrapper/container to wrap WS. Do not claim that an HTTP
pipeline authorizes a native WS upgrade.

In the native IDF backend, HTTP callbacks, WS callbacks and queued transmit jobs
execute on the same HTTPD task.
Application producers and ESP event callbacks belong to other tasks. Socket waits can
block HTTPD; an async-named native send does not guarantee nonblocking network I/O.
FreeRTOS preemption allows other ready tasks to run, not another HTTPD callback on
the busy HTTPD task. next() does not alter these rules.

WebSocketEndpoint/WebSocketClient and websocket_backend.h contain no SDK headers.
The shared endpoint owns admission, callbacks and broadcast; WebSocketSession owns
the copied-send token, while WebSocketExchange borrows one callback's receive data.
IDF keeps the native handshake/frame I/O/HTTPD queue in esp_idf_websocket.cpp.
SDK6 native WS requires CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y: opening uses
the post-handshake callback while the normal handler processes frames. Native6 WS
builds without that flag are rejected. Tested SDK5.4.0 does not use this option.
The older WindowsHttpBackend uses Winsock and platform SHA-1/Base64, with one serialized select loop;
memory supplies decoded messages and deterministic bounded send queues, not wire parsing.
See docs/readme_ws_adapters_ru.md for the contract and measured evidence.

The shared TCP backend uses the OS-independent handshake/frame codec and one
serialized HTTP/WS worker. WS admission prepares the session/client/container node
before 101; only full 101 allows opening dispatch through the original HandlerBase
and endpoint commit. Metadata is retained once at admission: actual raw head bytes
and an index for the actual header count. Callback Request/Response/client/data
views remain borrowed only for the callback. TCP defaults are four WS clients
per backend, four queued-or-inflight messages per client and 4096 bytes per message.
One shared 4×4096 B (16 KiB) TX pool is implemented per backend for all four WS
clients, rather than per-client payload storage. Queued/active frames own move-only
pool leases; blocks are released only on completion/abort. A full pool has no heap
fallback. Busy/GlobalFull retain the connection; individual QueueFull rejects new
sends and requests close1013 through the I/O owner while preserving a partial frame.
The P3 heap-queue ceilings remain historical.
An absolute output deadline starts before waiting for writability. Authenticated
WS is still unsupported; an HTTP Route does not authorize this separate upgrade.

Native IDF WS input defaults to4096 bytes. Dedicated URI contexts identify streams because IDF
resets the URI for subsequent frames. Producers copy payloads into ordinary heap and
queue jobs on HTTPD. Session tokens discard jobs after close even when a socket fd is
reused. Callback client/data pointers are borrowed only for the callback. Defaults are
four clients per endpoint and four pending messages per session; WebSocketLimits
configures these bounds. Client sends return WebSocketSendResult; broadcasts
return queued/rejected counts and the first error, without per-client storage.
Queued is admission, not delivery. This native queue path does not automatically
adopt the TCP pool/close-on-overflow policy. IDF stop rejects new jobs and drains
accepted jobs for up to five seconds
before httpd_stop; failure retains the backend for a retry. Three C3 restart cycles
and host queue/error paths passed; stop under continuous producer load and long-run
behavior remain unverified. Applications must coordinate producers and shutdown.

## Configuration and evidence

The selected product scope is a local controller with roughly 2–4 users/devices,
multiple sockets per browser, explicit resource limits and session close on stall or
individual overflow. SSE has its own implemented limits/deadlines; shared TCP has
bounded waits, an HTTP exchange deadline and a WS output deadline. Shared TCP
implements close-on-individual-overflow and a shared 16 KiB TX pool. Kconfig selects
native HTTPD (default) or TCP and separately WS/SSE; disabling SSE excludes its
encoder/queue/adapter. Native WS requires SDK HTTPD_WS_SUPPORT and, on SDK6, its
explicit post-handshake flag; TCP WS does not depend on HTTPD. WS admission results
are implemented; see docs/readme_static_ws_ru.md and docs/readme_tcp_http_ru.md for
behavior/evidence boundaries. Do not grow a slow-client QoS/replay/coalescing framework.

`examples/minimal` demonstrates public HTTP/WS and a private HTTP route via makeChain,
using static composition. `examples/compile_check` instantiates all three chain forms
without requiring integrated Wi-Fi. `examples/sse` demonstrates explicit EventSource
startup and authorized HTTP open. Demo UI sources are shared in root `frontend/`;
projects supply profiles/extensions rather than copied frontend/src trees.

Budget sockets rather than only browser tabs: preserve the owner's page during
acceptance, including its multiple connections. Native HTTPD preserves ordinary HTTP
connections by default; shared TCP closes them after one request. Native SSE
completion releases an async lease before the HTTPD task subsequently deletes its fd;
activeSSE is not the total SDK session count. These differences are not a proven
cause of a particular timeout without matching observations.

Write new or modified source comments in English. This does not request translation
of unrelated existing comments or source reformatting.

The six SeMaps views describe layers, ownership, execution contexts, WS/SSE memory
flows, application lifecycle and HTTP/SSE access. The 2026-10-24 revision includes
Request/Response, WS admission results and the library SSE worker/handoff/drain.
They are authored, not generated C++ models: graph_status reports no extractors.
See docs/readme_architecture_ru.md and the dated review linked there for verification
and live-versus-saved state. Save/discard still requires an explicit owner request.
Current pipeline usage: docs/readme_middleware_ru.md. New verification is reported in
docs/readme_middleware_verification_ru.md; historical baseline evidence remains in
docs/readme_verification_ru.md. The subsequent raw/copy body revision was flashed on the newly authorized C3:
13 loopback checks and network HTTP/auth/body/WS smoke passed, including a repeat
with the USB console closed. See docs/readme_request_body_verification_ru.md.
No load/soak or other-SoC runtime claim follows from these short checks.

HTTPS/WSS is a next-stage optional feature, not implemented. See
docs/ideas/IDEA_20261018_nawserver_optional-https.md. Future transport deadline handling
must respect esp_https_server ownership of send/receive/pending overrides, transport
contexts and open_fn; do not consume those hooks assuming plaintext-only sockets.

## Synchronous HTTP API

Request is read-only and Response is the sole synchronous writer for an exchange.
The dispatcher constructs both on the stack. EndpointRef supports SendResult(Request&,
Response&), SendResult(Response&) and middleware-aware Result(Context&), using the
same pipeline executor and access checks. Context now contains response. No generic
deferred HTTP API is provided. Response owns bounded mutable header storage; static
DefaultHeaders and ResponseOptions are borrowed/shared immutable configuration.
Bodies remain borrowed until synchronous send returns. TextBuffer replaces the old
owning response stream; native HTTPD/lwIP and request parameter maps may still allocate.
See docs/readme_http_api_ru.md and its verification report for migration and limits.
The provider owns Basic token verification; Request has no auth or sending methods.
WebSocketEndpoint/WebSocketClient use a queued-copy transport contract.

## Library SSE

Public sse.h exposes SDK-independent NaWServer::sse::EventSource, borrowing an
explicit sse::Backend from sse_backend.h. Canonical encoder/Sessions/bridge live
in src/sse and detail headers; tools/sse only tests/links them. IdfAdapter retains
native async HTTPD ownership and FreeRTOS worker. Its explicit header is
backends/esp_idf_sse.h. MemoryAdapter uses manual time and bounded capture;
WindowsAdapter runs one worker over nonblocking Winsock streams. TcpAdapter in
backends/tcp_sse.h borrows an explicit TcpHttpBackend identity and also reuses
QueuedAdapter/Sessions. Host HTTP exchanges expose owned SSE Streams, preserving
rollback before claim and transferring socket ownership only on activation.
No Request/Response/Next is retained. Component sources and esp_timer are explicit.
Constructors start nothing; start creates one bounded worker. Authorized Route
endpoints call open(Request&,Response&), publishers copy Frames into bounded queues
with try-lock, without waiting for the network. Queued is not delivery/ACK.
resolveHeaders -> owned preface -> reserve -> claimExternal -> activate gates first
bytes. Scoped rollback returns an unclaimed request to the callback; External errors
set an atomic abort flag. Request/Response/Next/callbacks do not survive handoff.
Backend reserve/activate are hidden from the normal EventSource API.

For TCP, reserve does no native I/O or close; rollback preserves HTTP ownership.
Activation commits a connection lease, but its SSE worker cannot send or close
until the HTTP callback has returned. Pending close is acknowledged only after
the underlying lease releases ownership; a retained native close failure leaves
the session active and drain unsuccessful. Stop producers, drain EventSource,
then end HTTP and stop the transport. TcpAdapter alone opts into restart after a
successful drain, preserving generations so old SSE tokens remain Closed. This
does not change the older adapters' restart behavior. EventSource may start before
HttpServer::begin; constructors start nothing.

Default limits: 4 slots, 3 pending/client, 768 encoded bytes/Frame, 2304 queued
bytes/client, 8192 total. Idle heartbeat and event stall/deadline are distinct.
Individual overflow/stall closes its session; global refusal does not blame a
client. Generation/serial reject stale transport steps. Stop publishers, then
successfully stop/drain EventSource before server.end. If stop(timeout_ms)
returns false, retain EventSource and the server and retry drain. Check the esp_err_t
from Server::end before destroying borrowed owners; HttpServer::end returns HttpError.
Windows and shared TCP HTTP end reject active SSE registrations so the owner can
drain and retry. Native IdfAdapter uses plain HTTPD in the tested 5.4.0/6.0.3 matrix;
TCP/IDF uses the separate TcpAdapter. TLS/send overrides/external forced close of
active async requests remain unsupported. WS limitations above are unchanged.
The IDF SSE worker sleeps at least one RTOS tick; converting 1 ms to zero ticks
at 100 Hz would starve lower-priority tasks. See docs/readme_sse_ru.md and
docs/readme_sse_adapters_ru.md; older verification reports remain historical.

## Connection admission (S3 application)

Server accepts ServerOptions::max_connections before begin(); default 7 is
unchanged. ResponseOptions::connection selects ConnectionPolicy::Close through a
typed setting; raw Connection headers remain rejected. This sets the response
header, not an unconditional native socket shutdown for arbitrary transports.
The S3 example explicitly closes its native rejection session after sending.
Admission policy, TCP reserve, WS/SSE caps, statistics and explanatory503 belong
to examples/touch_lcd7/main/network.*, not to the component. SDK pool is a separate
compile-time bound. See docs/readme_connection_limits_ru.md and the corresponding
20261019 connection-admission-and-reserve ADR. Hardware claims are S3-only.

## Application protocol and frontend

HTTP/WS transports carry bytes and do not impose a payload codec. Demo application
CBOR maps use UTF-8 field names directly, native CBOR values and bounded decoding;
unknown valid names need no registration. No numeric key registry/fallback belongs
in this design. Endpoint validation remains application work. Demo SSE carries JSON
text; EventSource itself provides framing rather than a JSON codec. See
docs/readme_cbor_requirements_ru.md and docs/readme_cbor_verification_ru.md.
Root frontend/ is the shared demo UI; LCD/product extensions, network admission
policy and data models remain application code. Existing C3/S3 reports are historical
evidence for their stated revisions, not fresh builds or hardware checks.

## TCP and IDF HTTPD layer map (2026-10-28)

The [NaWServer architecture](../docs/readme_architecture_ru.md) compares implementations, replacement boundaries and shared resources. `HttpServer` uses `HttpBackend`; the TCP path is `TcpHttpBackend` → `tcp::IdfTransport` → lwIP sockets, while the IDF path is `EspIdfHttpBackend` → `esp_http_server` → lwIP sockets. `Server` is specifically the IDF HTTPD facade. TCP transport can be substituted separately from the backend; HTTPD does not use it. Both paths use platform networking and FreeRTOS, while WS/SSE adapters and limits are not automatically interchangeable.

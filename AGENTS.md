# NaWServer: agent entry point

## Persistent API keys and page identity (owner decision, 2026-11-03)

Provide an optional platform-independent bounded key service with native storage
adapters, web and console management. Preserve Flash partitions, existing settings
and files. Persist and read back a new generation before publishing it; retain the
previous valid record. Do not mutate borrowed admission keys under live permits.
Public stable slot identities must never bypass secret validation. List operations
never return secrets; only explicit creation/rotation returns the new secret.
Each browser document generates its own random nonsecret page label, shared across
its HTTP/WS/SSE and distinct from another tab, including duplicated tabs. Keep
API-key and page-identity features optional, with NAWSERVER_ configuration flags.
TLS remains outside this task. S3 MAIN must not be flashed.

## Request streaming and progress (owner clarification, 2026-11-03)

The planned core revision must provide documented, first-class bounded buffered
and streaming request-body modes selected per route. Large bodies must not require
whole-body RAM. Waiting for body or TX space must yield the shared dispatcher;
a dedicated TX worker alone does not meet this requirement. The owner's latest
decision excludes a general deferred HTTP completion API: do not retain requests
waiting for arbitrary application results. Long application jobs return promptly
with a job identifier/status and use later polling or WS/SSE for results; their
application job pool is bounded too. This supersedes the earlier deferred HTTP
proposal. Bounded upload/download state, TX ownership and keep-alive remain.
Preserve simple bounded small-body handlers and serialized short application
callbacks. Do not silently parallelize existing callbacks. These are requirements,
not claims of implemented support.

## Routing and termination direction (owner decision, 2026-11-03)

Remove canHandle-based handler selection in the planned routing revision. The
owner explicitly requires no backward-compatibility layer for this unpublished
API: migrate repository consumers to declarative header-stage route selection.
Keep payload-based action dispatch an application/protocol choice. Do not claim
more URLs improve performance without matched measurements. Drop is an explicit
terminal state propagated through the processing chain and executed by the I/O
owner. Preserve keep-alive and managed connection/buffer machinery while moving
client quota policy out of QosHttp. The owner has now authorized implementation
and removal of native HTTPD: use the common HTTP/WS/SSE engine with platform socket
adapters. This supersedes older instructions below retaining HTTPD. TLS is future
work, not part of this removal. Keep optional OTA/log modules separate from core.

## Client admission policy ownership (owner decision, 2026-11-03)

New client IP/page/user/API-key quota decisions belong exclusively to middleware,
not TCP accept/transport. Run the optional early middleware stage after bounded
HTTP headers and before body allocation/read or WS/SSE commitment. Reuse one
policy/executor and propagate its result; do not add duplicate transport quota
tables or independent classification rules. Transport retains physical global
capacity, bounded I/O/timeouts and native handle ownership. A terminal middleware
Drop may request closure without an HTTP response; only the backend I/O owner
executes cancellation/close. Existing optional QosHttp admission is older code
to migrate, not evidence that new middleware-only admission is already shipped.
The current implementation plan is docs/plans/PLAN_20261103_client-resource-profiles.md.

## Configuration flag naming (owner requirement, 2026-11-03)

Every project-owned configuration flag/default macro and CMake cache option must
use the exact uppercase `NAWSERVER_` prefix. This applies to the library, optional
modules, backends, demos, tests and tools. ESP-IDF Kconfig symbols use
`NAWSERVER_`; their generated C/config names are `CONFIG_NAWSERVER_`.
Use `NAWSERVER_DEMO_` for demo-specific settings. Do not introduce mixed-case
`NaWServer_*`, shortened `NAW_*`, or unqualified project setting names.
When discussing flags or documenting commands, write their complete real names
(for example `NAWSERVER_TCP_ADMISSION`), never an ambiguous abbreviation such as
`TCP_ADMISSION`. Keep source, build definitions, presets, examples and maintained
documentation synchronized when renaming a flag. Do not rename public C++
namespaces, CMake targets or project names merely because they contain NaWServer.
Third-party compiler/OS/SDK-required identifiers retain their original names
(for example `CONFIG_FREERTOS_*`, `PICO_*`, `LWIP_*`, `YYJSON_*`); these are not
NaWServer-owned flags. Library-specific feature switches must remain distinguishable
from these external settings.
Obsolete spellings may appear only in migration diagnostics/tests, rename tables
and explicitly historical records; they must never remain accepted settings.

Publication baseline 2026-10-30: root documentation is Readme.md (English) and
Readme.ru.md (Russian). Keep their scope/contracts synchronized. The current
owner-authored code is MIT; preserve all separate vendor licenses/notices and
the dated provenance review. History restarts do not relicense old snapshots.
Canonical repository: https://github.com/NaBrIks/NaWServer, main. Public release
was authorized by the owner on 2026-11-07. Historical local refs/stash are
recovery material: never publish them with --all/--mirror or automatic tag pushes.

Owner memory/connection refinement 2026-10-27: managed TCP buffers allocate only
the configured initial count, grow within byte/slot/free-memory ceilings and
release dynamic blocks immediately. Physical primary API buffers are opt-in,
separate from admission quotas. No HTTP body allocation for bodyless requests;
WS/SSE payload storage is lazy. Preserve move-owned/immutable output lifetimes,
central readiness reasons and bounded sequential HTTP/1.1 keep-alive without
retaining work buffers between requests. These options also work without an
admission policy on Windows/Linux. See docs/readme_buffer_memory_ru.md and
docs/readme_20261027_memory_manager_keepalive_ru.md. This supersedes the previous
all-jobs/all-WS-arenas startup reservation, not the serialized callback contract.

Owner QoS clarification 2026-10-27: keep admission optional and independent,
with defines/static configuration suitable for MCUs. A configured primary key
owns protected capacity; an application-supplied IPv4 hint only prioritizes TCP
ingress and never bypasses key/auth checks. Read docs/readme_admission_ru.md and
docs/readme_20261027_admission_qos_ru.md before changing this contract. Preserve
serial callbacks, explicit immutable-body lifetime, bounded rejection storage,
and the distinction between HTTP503/429 and exhaustion of raw TCP ingress.

TCP follow-up evidence 2026-10-27: IDF TCP_NODELAY and public bounded close
grace/discard options address the observed WS truncation and premature HTTP
close. Read docs/readme_20261027_tcp_rest_ws_close_ru.md before interpreting
the older dial/WS-close failures. Keep source-port reuse/TIME_WAIT distinct
from Wi-Fi loss, short normal REST acceptance distinct from reload overload,
and Close-frame arrival distinct from TCP EOF. Do not reinstate SHUT_WR before
the owned native close or reenter application WS handlers during HTTP output.

Owner decision 2026-10-27: public memory/resource configuration must not know
about demos, LCD or board-specific budgets. TCP HTTP/WS and SSE have separate
buffer allocators and hard reserved-byte ledgers; ESP-IDF placement policies
stay in its backend helper. JSON/MCP workspace remains caller-owned. Preserve
strict versus preferred/fallback semantics and the max_align_t contract (IDF
capability malloc alone guarantees only four bytes). Test normal REST automation
with 1/2/3 sequential clients separately from short UI visits and overload. Do
not declare latency acceptance from increased RAM or a successful retry. Read
docs/readme_buffer_memory_ru.md and docs/readme_20261027_memory_limits_tests_ru.md.

Owner SDK clarification 2026-10-26: the shared versioned SDK layout and tool
versions are a recommended reproducible baseline, not a mandatory machine layout.
Use username-independent paths. Explicit alternative paths/version profiles are
allowed with separate validation; do not silently change versions during checks.
Read docs/readme_development_environment_ru.md and docs/readme_shared_idf.md.
Append measured ESP configuration/Flash/RAM records to docs/readme_build_memory_log_ru.md.

Owner decision 2026-10-20: demo frontend is shared in root `frontend/`, based on
the current S3 UI. Follow frontend/AGENTS.md. Demo projects add only profiles
and project-specific extensions; do not restore copied frontend/src trees.

ESP-IDF HTTP, WebSocket and SSE component.

Owner MCP direction 2026-10-25: the runtime must be fully autonomous inside the
controller, with clients connecting directly to its MCP endpoint. A host Node.js
bridge is not the target architecture. Plan a common bounded protocol/registry/
result mechanism above HTTP, with static tool definitions and application-owned
handlers. Keep application/UI logic out of that common mechanism. This records
the direction. Embedded MCP is an opt-in C++ endpoint with compile-time JSON
reader selection (bounded, yyjson or RapidJSON SAX), a shared bounded writer and caller-owned
storage. Read docs/readme_json_backends_ru.md for settings, ownership and reuse;
docs/readme_mcp_implementation_ru.md records protocol and acceptance limits.
The module must be build-time optional. Owner examples are `NaWServer_MCP`
and `NaWServer_MCP_CJSON`; these do not select cJSON as the final backend.
JSON reading/writing needs a small replaceable contract with explicit ownership
and MCU bounds; include an independently authored C/C++ backend among candidates.
Read docs/readme_mcp_json_requirements_ru.md before MCP/JSON design or edits.

Owner TCP engine decision 2026-10-24: plan a shared HTTP/WS wire engine over a
narrow TCP boundary for Windows, Linux and ESP32, retaining selectable native
IDF HTTPD. Support IDF 5 and 6 through an explicit tested-version matrix; C3 is
the MCU acceptance target. Avoid unnecessary copies/allocations in our code;
account for KiB-scale buffers, queues, stacks and build dependencies. This
supersedes the IDF-only backend restriction, not application boundaries or the
ban on a universal HAL. Shared TCP HTTP/WS/SSE and bounded storage now pass P5 on
Windows and isolated Linux. P6 IDF adapters, backend selection and exact SDK5/6
builds are implemented. All four C3 SDK/backend profiles pass on-device and
explicit LAN/SSE probes; intermittent browser WS/SSE failures remain under
investigation, including native baseline. P6 and full MCU/load/soak acceptance
remain open. Current evidence is in
docs/readme_tcp_engine_verification_ru.md. Before continuing,
read docs/adr/ADR_20261024_nawserver_shared-tcp-http-engine.md,
docs/reviews/2026-10-24_shared-tcp-http-engine.md and both linked implementation
and contract plans. Full acceptance includes MCU-appropriate load and soak tests.

Owner HTTP experiment 2026-10-24: implement a shared synchronous HTTP path with
in-memory and Windows backends, then migrate IDF. Keep existing request processing
semantics and leave small utilities/decoders alone. Run ESP32 builds in the
background alongside independent work. The owner authorized C3 hardware acceptance
on 2026-10-24. This authorizes a narrow HTTP backend contract, not a
universal HAL. See docs/readme_http_api_ru.md and its dated verification section.

Owner protocol decision 2026-10-20: application CBOR maps carry UTF-8 field names
directly. No numeric key IDs, cbor-keys.json, generated registries or equivalent
name-to-number schemas. Unknown valid names need no registration. Preserve native
CBOR values, bounded decoding and endpoint validation. Read docs/readme_cbor_requirements_ru.md.
Read [agents/architecture.md](agents/architecture.md), [docs/README.md](docs/README.md)
and the current verification report before edits.

- Python is forbidden for agent-authored tools and automation. Do not write or run
  Python scripts, including temporary one-off helpers. Use Go for small tools.
  Existing vendor build dependencies do not justify adding Python glue.
- Keep HTTP/WS independent of Wi-Fi initialization, credentials, BLE, TPMS, LCD,
  storage mounts and product pages. Retain native IDF HTTPD alongside the planned
  shared TCP engine; do not invent a universal HAL.
- Work only in this repository; do not change other projects without an explicit request.
- Use clear, complete names in project code. Avoid abbreviations unless they
  materially improve readability or follow an established protocol/technical term.
  Prefer session_context to sess_ctx and native_request to req; a few saved
  characters do not justify an obscure name. Keep mandatory SDK/API identifiers
  unchanged; do not introduce macros merely to rename external fields.
- Explain non-obvious decisions and architectural ownership/lifecycle boundaries
  in English code comments. Describe the reason and constraints, not only the
  adjacent statements. Current owner guidance (2026-10-25): keep development fuzz
    runs to at most two minutes during daytime (latest owner refinement);
    long-duration load testing is deferred to the owner's night-time run.
- Use explicit targets and the pinned sample configuration. Builds and hardware
  operation are separate evidence; "all ESP32" remains a target until verified.
- Current owner scope: ESP32-C3 only (2026-10-18). Build and test C3 while developing;
  defer other ESP32 target builds, matrix checks and hardware work until requested.
  Preserve SoC-independent architecture without spending time validating other targets now.
- Owner priority clarification (2026-10-25): first deliver a working MVP on C3.
  Use Windows and isolated Linux for quick builds and surface regression checks;
  defer broad host matrices and extended investigations until the C3 MVP works.
- Handlers must outlive `Server::end()`. Configure routes before `begin()`.
  Coordinate sends and shutdown in the application. Never stop inside an HTTP callback.
- Secrets, firmware binaries, flash backups and raw serial logs stay ignored.
- Preserve `LICENSE`. Do not publish,
  commit, push, set a remote or erase hardware unless requested.
- The owner authorized experiments only on the identified C3. Re-identify the
  current port/chip/device before flash. Owner clarification (2026-10-19): do not
  make flash backups; the previous mandatory backup/SHA-256 prerequisite is removed.
  Verify intended flash write ranges. Owner clarification (2026-10-24): do not add
  separate post-flash hash/readback passes; keep the vendor's normal write behavior.
- Public headers live in `include/NaWServer`; applications use `<NaWServer/...>`.
  Keep implementation directories private. SSE's concrete static storage requires
  implementation headers in `include/NaWServer/detail/sse`; these are not application API.
- SSE runtime lives only in `src/sse`, public `NaWServer/sse.h` / `sse::EventSource`.
  Keep tools/sse as tests/fixtures; never restore removed implementation snapshots.
- Owner SSE portability scope 2026-10-24: EventSource borrows an explicit Backend;
  Memory, Windows and IDF adapters reuse the bounded encoder/Sessions/bridge.
  Host adapter/HTTP stream bindings live in src/backends. Keep other portability
  findings unchanged during this SSE stage. Validate browser Windows and C3;
  run SDK builds in the background.
- SSE start is explicit after SDK init; auth/policy precedes open through Route.
  Stop producers and drain EventSource before server.end(). IDF is plain HTTPD only,
  no TLS/send overrides/external forced close. No Request/Response/Next is retained.
- Authenticated WS remains unsupported until pre-handshake checks have tests. Native
  WS and protected HTTP Route use separate paths; an HTTP pipeline does not protect
  a WS upgrade. The legacy auth wrappers and container have been removed.

Documentation lifecycle: current rules in `agents/` (English), guides
in `docs/readme_*`, immutable `docs/adr/ADR_YYYYMMDD_zone_short-name.md`, editable
`docs/plans/PLAN_YYYYMMDD_zone_short-name.md`. Supersede decisions with a new ADR.

## Document languages and translation

- For paired documents, `name.ru.md` is the owner's Russian source and `name.md`
  is the English version used by agents. Read and use the version without `.ru`.
- At the start of work involving a paired document, and again after owner edits,
  compare their last-write times. A newer Russian file is a signal to check the
  content and update the English translation before relying on it.
- File dates are hints, not proof that translations match: checkout and copying
  can change them. Compare content when dates are unclear or a conflict is found.
  The Russian source takes precedence when the two versions differ.
- When the owner says "переведи" (translate), translate the relevant Russian
  source into its English partner. Preserve meaning, code formatting, identifiers,
  paths and historical claims. Do not change technical decisions while translating.
- Keep paired versions in sync when making authorized documentation changes.
  Do not overwrite owner edits in Russian with an older English version.
- Current pairs: `agents/architecture.ru.md` / `agents/architecture.md` and
  `agents/code_style.ru.md` / `agents/code_style.md`. Existing `_ru.md` guides and
  immutable ADRs keep their current names; this rule does not rename them.

## Source code style

Before writing or formatting source code, read [agents/code_style.md](agents/code_style.md).
Follow its examples and notes, including owner edits. Do not reformat the whole
repository unless explicitly requested.

## SeMaps authoring

- Use the registered SeMaps MCP tools for the model, never edit registry/view files directly or create a temporary protocol client. Check graph_status before relying on extracted code facts; no C++ extractor is currently configured.
- The owner requested the six architecture views. Read layout_guide before geometry changes, verify with get_view/render_view, and save/discard only after an explicit owner request.
- Current architecture: docs/readme_architecture_ru.md. The next pipeline and fixed Basic provider are implemented; dynamic stores/roles and CONFIG_NAWSERVER_* remain ideas.

## New API requirements (owner decision, 2026-10-18)

- No legacy middleware/API compatibility layer. Migrate independent examples with
  the new API; do not preserve old body/upload callbacks or decorator forwarding.
- Support static composition of server, route descriptors, guards and providers.
  Do not require heap factories, mutable route registration or a DI container.
- Constructors bind dependencies; explicit init/start loads storage and starts
  runtime resources after SDK prerequisites. Static ownership is not a zero-heap
  promise for HTTPD/lwIP or the whole firmware.
- Runtime users/roles are bounded data. Permission/operation definitions are fixed
  by firmware; dynamic RBAC is optional and does not require runtime code generation.
- The owner decision is docs/adr/ADR_20261018_nawserver_static-composition-and-clean-api.md.
  Historical review suggestions of a legacy adapter are superseded.

## Middleware direction selected (owner, 2026-10-18)

- Use a small next-based pipeline with static chains of different lengths, including
  an empty chain. Keep declarative route access policies within this mechanism.
- Support typed Chain<MiddlewareTypes...>, makeChain(instanceRefs...), and a counted
  FixedChain<N> facade over one common executor. Prefer typed/factory usage in examples.
  No per-request allocation is required by the pipeline itself.
- Keep static-memory/binary/gzip/file/source serving flat and explicit; middleware
  is optional around the endpoint, not an obligatory hierarchy for public assets.
- next is valid only during the call and may run at most once. Deferred transport
  uses a separate owned lifetime/completion contract, not a retained stack Next.
- Bound producer queue/pool, distinguish queued from delivered, and test simultaneous
  static transfers and WS under slow readers. FreeRTOS preemption does not let a busy
  HTTPD task dispatch another job before its current callback returns.
- See docs/adr/ADR_20261018-2_nawserver_static-next-pipeline.md. The engine is implemented; see docs/readme_middleware_ru.md and its separate verification report.

## Chain API preference and backpressure proposal

The owner requested typed Chain<AuthMiddleware, AccessMiddleware>, makeChain(auth,
access), and a counted fixed form. Use one engine; factories and typed wrappers
must not fork execution semantics. A counted class needs a different C++ name
(e.g. FixedChain<N>). Middleware and chain references are non-owning by default;
reject dangling temporary middleware. See docs/ideas/IDEA_20261018_nawserver_chain-facades-and-backpressure.md.
The owner later rejected elaborate slow-client support; the local-device limits ADR
supersedes those policy proposals. Queue pressure alone does not identify a slow client.

## Local device scope (owner, 2026-10-18)

- Typical use is local LAN/phone setup with 2–4 clients. Use simple explicit bounded
  code configuration. Separate max TCP connections from max WS clients because a
  browser may open multiple sockets. WS-client starting target is 4, configurable.
- Close a timed-out/stalled connection or one exceeding its individual pending limit;
  release resources. Reject excess new clients. Never wait for a slow consumer in
  the application producer, grow an unbounded queue, or silently lose command replies.
- Do not add default slow-client worker pools, adaptive QoS, replay/history or
  automatic snapshot coalescing to the core. Changes for normal-load responsiveness
  require measured need. Timeout is a session close, not a permanent IP/user ban.
- Use a socket wait bound and a total transfer deadline with bounded backend steps;
  do not claim an outer middleware guard interrupts an arbitrary blocking send.
- See docs/adr/ADR_20261018-3_nawserver_local-device-limits.md; it supersedes elaborate
  backpressure proposals in the preceding IDEA. The pipeline is implemented; new transport limits are not implemented yet.

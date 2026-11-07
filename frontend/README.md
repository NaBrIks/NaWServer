# Optional device client and Vue interface

The HTTP server does not require this frontend or its device telemetry API.
The files here form three separately usable layers:

| Entry | Purpose | Vue required |
| --- | --- | --- |
| `src/contracts/` | CBOR codec and v2 demo/device telemetry types and validators | No |
| `src/client/index.ts` | Browser HTTP/CBOR request, WebSocket and EventSource factory | No |
| `src/kit/index.ts` | Ready TechnicalShell and optional Vue components | Yes |

The client imports without starting network activity. Construct `DeviceClient`
with an optional absolute `baseUrl`, timeout, response byte limit or custom fetch.
`request()` returns HTTP status and challenge/retry metadata even on HTTP errors.
Transport/decode/limit failures reject. Caller cancellation and the client timeout
both remain active. Response bodies are capped while reading, not after an
unbounded `arrayBuffer()`. WebSocket/EventSource are native browser handles:
the caller owns their listeners, retry policy and `close()` lifetime. The shared
Vue state supplies the showcase's bounded histories and reconnect behavior.

```ts
import {DeviceClient,isInfo} from './frontend/src/client/index.ts';
const client=new DeviceClient({baseUrl:'http://192.0.2.10/'});
const result=await client.request('/api/v2/info');
if(result.ok&&isInfo(result.data))console.log(result.data.chip.model);
```

These v2 endpoints describe the optional device showcase contract. They are not
mandatory endpoints of every NaWServer application. Other frameworks can consume
the client/contracts, and other languages can implement the documented wire
formats without a Vue dependency. CBOR field names remain direct UTF-8 strings.

## Project profiles

One maintained shared source tree is built for every demo. A project adds
`examples/<name>/frontend/project.ts`, `tsconfig.json`, and optional extension
components. Vite selects an existing profile by `--mode <name>`; it performs no
source text replacement. Profiles own product labels, SDK display name, memory
label, workload safety maximum, peripheral controller label and detail labels.
Device information/capabilities remain authoritative for actual hardware and
available operations. `idf_version` is the legacy v2 field name for SDK version;
the profile changes its caption, not the reported value.

Run `node frontend/tools/build.mjs <name>` for one profile or `npm run build` for
the four maintained profiles: showcase, touch_lcd7, c6_lcd147, rp2350_pico2_w.
No copied shared Vue components/styles or separate dependency lock are needed.
On a fresh checkout run `npm ci`, `npm run build`, then `npm test` at the
repository root: the shared-build tests inspect the generated profile assets.
For development proxying, set `NAWSERVER_DEVICE_URL` to the actual device URL.

The exported CPU panel accepts core values/history as arrays and a separately
reported total. It does not assume two cores or synthesize unavailable values.
The optional `summary.data.transmission` field adds the shared transmitter to
the same INFO overview. `TransmissionPanel` displays actual configured job/byte
limits, per-connection allowance, important reserve, filling/queued/active jobs,
memory usage/allocation/peak, outcome counters and accepted/sent/discarded/failed
bytes. Jobs/memory histories are bounded to 600 samples. TX rate uses actual
sample timestamps and sent-byte deltas; reset or unsafe deltas remain unavailable.
Exact uint64 totals remain bigint. `sent` means TCP-stack acceptance, not a peer
ACK. The limits are read-only telemetry; this module does not invent a settings
mutation endpoint. Old firmware without this field keeps its existing overview.
Optional `reserved_important_bytes` and `borrowed_bytes` occupy permanent compact
slots in the existing totals strip, using the same KiB units. Missing values stay
unknown. Borrowed counts current externally owned immutable data, not allocated
payload RAM; reserve is the configured protected byte allowance.
Optional `step_bytes`, `stall_timeout_ms` and `close_timeout_ms` show the actual
transmitter step and timeout configuration in the existing details dialog.
Optional `last_failure` (0 none, 1 expired, 2 stalled, 3 native write, 4 canceled)
and signed `last_native_error` are shown in the panel's existing details dialog.
Their arrival or change adds no rows to the overview and opens nothing automatically.
The shell's message indicator occupies a permanent footer slot. Full messages
open only by user action; history is bounded to 32 items and retained until
cleared or replaced by later bounded history. No toast/banner opens automatically.

## Independent observer channels

The existing header has three independent fixed-width controls:

| Control | Default | Network activity |
| --- | --- | --- |
| WS INFO | On | One WebSocket, live summary messages and optional echo |
| HTTP 1 Hz | Off | Explicit opt-in summary plus every paginated task page, at most once per second after each completed snapshot |
| SSE | On | One independent diagnostic event stream |

Page startup requests one `/api/v2/info`, one summary and one complete paginated
task snapshot. The refresh icon repeats that explicit snapshot. The optional
HTTP observer requests summary/task pages, without repeatedly fetching info.
With HTTP observation off there is no background summary/tasks polling and no
HTTP fallback when WS is disabled or disconnected. WS boot changes do not
silently fetch info. Manual refresh/page reload updates its metadata explicitly.

Turning a channel off closes its connection, cancels retry timers, and suppresses
reconnect until it is enabled. HTTP disable also aborts its in-flight automatic
snapshot. Preferences are stored per device origin and project profile and sync
between tabs through storage events. Initial info and manual refresh remain
available with every continuous observer off.

With WS and HTTP off, INFO retains its last values and the existing status reads
Snapshot; SSE does not silently overwrite the full INFO summary. Tasks are labeled
as an HTTP snapshot unless HTTP observation is enabled. Snapshot values remain
visible after six seconds; missing/stale live channels keep their existing
fixed-place status. No notification opens automatically.

Queued TX jobs already include active writing jobs; the occupied-job history is
`queued_jobs + filling_jobs`, without adding `active_jobs` a second time.

## Validation of the profile extraction (2026-11-02)

Type checks and production builds pass for all four profiles. All 34 frontend
tests pass: the previous 24, four client tests, three transmitter contract/rate
tests and three observer preference/lifecycle tests. A controlled headless Chromium browser with recorded RP information and
synthetic WS transmitter samples verified RU/Cream and EN/Dark, all core/total
traces, live TX changes and explicit-only message details. CPU/RAM panel and
footer rectangles and content scroll position were identical before error,
after error, after success and after clearing messages. No browser page errors
were observed. Evidence is ignored under `local/frontend-profile-check/`.
This controlled browser check does not claim that updated firmware was flashed
or that live board measurements were validated; hardware acceptance is separate.

`node frontend/tests/observer-browser.mjs` starts its own local Vite/API fixture
and isolated headless browser. It verifies actual native WS and SSE connections,
independent close/re-enable, reconnect, persistence and cross-tab changes, manual
snapshot counts, and that HTTP observation neither starts by default nor
continues after disable. The fixture has nine tasks (two pages): startup remains
exactly info=1, summary=1, tasks=2 while WS/SSE run. One manual refresh adds
exactly one info/summary and two task requests. Geometry is unchanged across
channel switches. Evidence is ignored under `local/observer-browser/`.
Use `NAWSERVER_BROWSER_EXECUTABLE` to select an existing browser binary if needed;
the probe does not install a browser or contact a physical board.

## Persistent API keys and page identities (2026-11-03)

The browser client labels all HTTP requests with `X-Page-Id` and native WS/SSE
URLs with the same non-secret `page_id`. A cryptographic random label is generated
once per document: clients in one page share it, reload and duplicated/new tabs
get fresh labels. No local/session storage is used for this identity. Use
`new DeviceClient({pageIdentity:false})` for endpoints that do not use page quotas
or where cross-origin custom-header preflight is undesirable. `pageHeaders` is
available for custom fetch code. Importing the client starts no network traffic.

The Access page includes bounded API-key metadata and explicit create/rotate/
revoke actions protected by its current session credentials. Names have a24-byte
UTF-8 limit; creation uses the first free device slot. Secrets are displayed only
in the dialog opened by create/rotate, cleared on close/navigation, and copied
only by explicit action. The client never places an API secret in a URL or
browser storage. Devices retain the keys; the page retains neither a readable
copy after dismissal nor a restore mechanism. All configured slots are visible
in one table. Operation feedback uses the existing fixed footer indicator.

`node frontend/tests/page-keys-browser.mjs` uses a controlled local API/browser
fixture to test metadata rows, unchanged geometry after401/success, rotation,
revocation, secret dismissal and fresh reload/tab identities. It does not flash
or change any physical device. `node frontend/tests/observer-browser.mjs` also
covers native WS/SSE URLs carrying the new page label.

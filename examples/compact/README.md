# Compact device demo

A separate native C++ application for a small real device: one persistent API
key, two simultaneous SSE status observers, three settings and a streamed A/B
firmware upload.
It uses the existing common NaWServer engine without changing library sources.
The web page has no Vue, package installation, images, fonts or external service.

| Directory | Responsibility |
|---|---|
| `common` | Public management routes, keyed automation routes, bounded JSON, telemetry and OTA response |
| `web` | One page, incremental browser SHA-256 and build-time gzip packer |
| `platform` | Native persistent settings, key storage and firmware adapters |
| `esp` | ESP-IDF application, build profiles and partition tables |
| `rp` | Pico SDK / FreeRTOS application, ROM A/B and strict UF2 validation |
| `host` | Windows/Linux API harness; firmware writing is explicitly unsupported |
| `tests` | Native host checks and explicit ESP hardware OTA acceptance |

## Protocol and limits

SSE is the selected status transport. Only the selected protocol is linked.
The alternate `NAWSERVER_DEMO_STATUS_WS` build is a binary-size comparison;
the supplied browser page implements SSE, not the alternate WebSocket client.
Each page opens one initial status request and then a public management fetch
stream, without periodic HTTP polling. Two browser documents use independent
SSE connections. Settings and firmware use ordinary HTTP POST; the manual
counter-refresh button reads `/ui/metrics` once.
The diagnostic page is **19,916 bytes before gzip / 7,773 bytes after gzip**.
The earlier matched `0.1.2` C3 firmware builds used 963,904 bytes with SSE and 973,920 bytes with WS:
SSE saves 10,016 bytes of Flash.
ESP builds use the SDK performance optimization
profile; Pico is built as Release. Assertions remain enabled in the ESP profile.

| Request | Access / body | Result |
|---|---|---|
| `GET /` | Public, gzip-capable browser | Embedded static page |
| `GET /api/status` | `X-Client-Key` | Current settings, heap, firmware and slot |
| `GET /api/events` | `X-Client-Key`, shared two-observer limit | SSE JSON status records |
| `POST /api/config` | Key, `application/json`, at most 256 bytes | Persist and return settings |
| `POST /api/firmware` | Key, `application/octet-stream`, SHA-256 header | Stream to inactive slot; 202 then reboot |
| `GET /ui/status`, `GET /ui/events` | Public management, no API key | Same status and shared observer limit |
| `POST /ui/config`, `POST /ui/firmware` | Public management, same body limits | Same settings and native update implementation |
| `POST /ui/api-key` | Public management, explicit action | Persist replacement key and return it once with 201 |
| `GET /ui/metrics` | Public management, no API key | Status plus cumulative CPU and request/stream counters |

This demo deliberately has **no web login**. The page connects automatically and
uses only `/ui/*`; it never requires or sends the automation credential.
The `/api/*` routes require `X-Client-Key` for external integrations. The public
management routes are accessible to other LAN clients too: API-key enforcement
on `/api/*` does not add authentication to the explicitly public management UI.

Configuration is exactly `{"name":"Device","interval_ms":2000,"led":false}`.
Names occupy at most 24 UTF-8 bytes and contain no control characters. Periods
range from 1000 to 60000 ms. Unknown/duplicate/missing fields are rejected.
ESP stores the LED flag as an example setting; no board-specific LED GPIO is
selected there. Pico applies it to the built-in wireless-chip LED.

One key is provisioned in native persistent storage. Use the page's explicit
create/replace button, copy the returned value and put it in the integration's
`X-Client-Key` header. An empty store also prints its initial key on the physical
console; physical `k` rotates and prints a replacement. Replacement revokes the
old key. Reloading the page neither rotates nor retrieves a stored secret; a lost
credential can be replaced explicitly. The browser keeps the newly issued value
only in page memory, without secret URLs or browser-storage credentials. Rotation
terminates streams admitted with the previous API-key revision while public
management streams continue. API and UI streams share two slots; a third
observer receives 503. A disconnected slot can be reused with a new generation.
Browser requests with an Origin header must originate from the device itself.
This profile uses plain HTTP on port 80, so its key does not provide encryption.

See [measured CPU/RAM scenarios and qualified historical comparisons](MEASUREMENTS.md).
The page displays allocator names, free/total KiB, free percentage, largest block
and active-core CPU. RP exposes its RTOS and C++ arenas separately. `/ui/metrics`
adds cumulative CPU and request/stream counters for clean boundary measurements;
the page itself does not poll that endpoint.

The request stream block is **1024 bytes**, not a firmware-size limit. Settings
use a separate 256-byte buffered-body limit. TX has four 1024-byte blocks and
a 4096-byte ceiling; there are two HTTP work slots and four physical connections.
The configured TCP buffer ceiling is 24 KiB. SSE has two clients, one pending
record per client, a 768-byte per-client limit, a 1536-byte aggregate pending
limit and a 4096-byte SSE buffer-memory ceiling. With the shared direct-TX path,
SSE payload leases come from TX rather than a separate frame-pool allocation.
These are limits, not a claim that every buffer is allocated at startup.
The common upload deadline defaults to 120 seconds and can be overridden with
`NAWSERVER_DEMO_UPLOAD_TIMEOUT_MS` for a measured platform profile. Header and
progress deadlines are configured separately. This tiny profile closes ordinary
HTTP connections; it does not enable the full diagnostic demo's keep-alive profile.

The normal resource workload is one sequential API client plus repeated page
downloads alongside two persistent SSE observers. Four physical connections
allow those four active sockets; extra concurrent probes or partial uploads are
an overload scenario, not another guaranteed normal client.

## TX payload memory modes

`NAWSERVER_DEMO_TX_MEMORY_MODE` selects the same bounded transmission mechanism
with different payload storage. Default block settings are
`NAWSERVER_DEMO_TX_BLOCKS=4` and `NAWSERVER_DEMO_TX_BLOCK_BYTES=1024`.

| Mode | Payload at startup, default profile | Payload lifetime |
|---|---:|---|
| `0`, lazy heap | 0 bytes | Actual-size blocks allocate on demand and release after completion/cancel |
| `1`, preallocated heap | 4096 bytes | Fixed blocks remain allocated for the TX runtime lifetime |
| `2`, static RAM | 4096 bytes in application BSS | Caller-owned array remains reserved for the firmware lifetime |

These modes describe **TX payload only**. Connection/job tables, HTTP workspaces,
task stacks, synchronization, SDK/Wi-Fi/lwIP and allocator overhead remain separate.
Static storage moves payload out of heap; it does not remove that RAM cost.
`tx_bytes` includes allocated static or heap payload; `tx_reserved_bytes` is
already included in it. `tx_limit_bytes` is a ceiling, not an additional reserve.
The 24 KiB TCP buffer ceiling likewise does not mean 24 KiB is preallocated.
`tx_failures` combines rejected and failed queue jobs, including retryable
backpressure; it is not a count of lost HTTP replies or SSE deliveries.

Blocks must be 768..4096 bytes and a multiple of 16: the direct SSE path needs
one complete bounded frame in a lease. There are 2..12 configurable blocks;
passing configuration validation alone does not establish the two-stream load
acceptance of every smaller pool. Platform build commands and additional SDK
memory profiles are documented in the [ESP](esp/README.md) and [Pico](rp/README.md)
guides. The S3 HTTP worker uses 8192 bytes after a 6144-byte overflow; this does
not prove that 8192 is the smallest safe stack for all callbacks.

## Native firmware update

See [ESP build and storage instructions](esp/README.md) and
[Pico build and storage instructions](rp/README.md) before initial installation.
Initial USB installation of a new partition table is distinct from routine OTA.
Do not apply a table based only on a chip name. Existing FS/NVS bounds must match.
The owner explicitly authorized the 2026-11-03 installation on the identified
shared S3 while retaining all NVS and project-filesystem data. This is not
permission to erase another board or change retained data partitions.

ESP receives the application `.bin`. RP receives only the dedicated `.ota.uf2`;
the ordinary SDK UF2 may include an absolute guard block outside the image slots.
The browser hashes the complete original file incrementally, then uploads it
directly with `X-Firmware-SHA256`. The MCU never buffers the whole file. The current
firmware remains the recovery image while the other slot is overwritten.
Native image checks and the complete digest must pass before boot selection.
Accepted candidates cannot be overwritten by another upload before reboot.
After the response is queued, status production stops so TX can drain.

Hash validation detects corruption; it is not firmware signing or secure boot.
Power-loss recovery relies on the platform's native image validation/rollback,
not on an assumption that writing the candidate's final sector is atomic.

## Build and repeatable checks

SDKs/toolchains must already exist. The platform build scripts do not download
dependencies. Vendor ESP-IDF Python tools are used; application/automation source
is native C++, JavaScript, CMake or PowerShell.

For the native host harness, use a normal compiler environment:

```sh
cmake -S examples/compact/host -B examples/compact/host/build -DCMAKE_BUILD_TYPE=Release
cmake --build examples/compact/host/build --config Release
ctest --test-dir examples/compact/host/build -C Release --output-on-failure
node --test examples/compact/web/hash.test.mjs
node examples/compact/web/browser-test.mjs
```

The browser check uses an already installed Playwright/browser and a local HTTP
fixture. It checks stable light/dark/mobile layouts, automatic connection without
a key, explicit key creation/copy, no rotation on reload, configuration,
SSE without polling and a 64 KiB upload with the correct digest.
It also checks two independent documents receiving SSE, closure of one document,
stale/reconnected status, the three TX memory-mode labels and manual-only metrics.
It does not flash a physical device. The host test checks the real native server,
including SSE plus concurrent HTTP, key rotation, distinct API/UI stream lifetimes
and graceful shutdown. Its isolated key store
is created in the OS temporary directory, never in version-controlled files.

Explicit ESP hardware acceptance (this **writes and reboots** the supplied device):

```sh
node examples/compact/tests/esp-ota-accept.mjs BASE_URL PRIVATE_KEY_FILE A_BIN B_BIN PRIVATE_REPORT_FILE
```

Start on image A; build B with a different version string. The test rejects a bad
digest and a truncated body, updates A→B→A, waits for health confirmation and checks
that the key and settings survive. Keep reports and device credentials ignored.

For a normal resource run without writing firmware:

```sh
node examples/compact/tests/resource-load.mjs http://DEVICE PRIVATE_KEY_FILE PRIVATE_REPORT 60 --label=PROFILE
```

The runner opens two SSE observers and exercises API requests and page downloads.
It takes periodic resource samples from SSE, checks both event counts and the
final silent interval, and saves latency/memory counters. `--slow` pauses the
second receiver; this alone does not prove remote TCP buffers filled.
`--overload` adds aborted requests and treats bounded refusals separately.
`--ota=IMAGE` explicitly writes the inactive slot and reboots; `--baseline=REPORT`
compares a passing compatible workload. Runtime reports, images and credentials
remain ignored. A short passing run does not establish long-duration stability.

## Current two-observer checks, 2026-11-03

The S3 baseline with an 8192-byte HTTP stack passed a 30-second normal run:
15 events reached each observer, 29 API requests and 14 page downloads completed
without errors, and the largest observed SSE gap was 2023 ms at a 2000 ms period.
The two-core mean CPU was 2.89%. This is normal-load HTTP/SSE evidence, not S3 OTA
acceptance or a proof of the minimum safe stack.

Pico's 160 KiB RTOS/lazy-TX baseline passed a 45-second normal run: 22 events per
observer, 43 API requests and 21 page downloads without errors, with a largest
SSE gap of 2028 ms at a 2000 ms period. Mean single-core CPU was 4.54%.
See [resource measurements](MEASUREMENTS.md) for memory minima and the scope of
these results. No equivalent current C3 result or complete profile matrix is
implied by these two runs.

## Earlier single-observer acceptance recorded 2026-11-03

The final `0.1.2-compact` C3 public-management profile was installed and checked
in an actual browser. Opening it needs no key; explicit creation returns 201,
the new key authorizes `/api/*`, and the old key returns 401. The public SSE
stream continues during rotation. A subsequent OTA kept the issued key and
settings. Both final OTA phases took about 22 seconds. The final C3 application
is 963,904 bytes, with approximately 168,000 free heap bytes during normal use.
The ESP32 / C6 / S3 builds are 893,824 / 1,016,048 / 889,328 bytes respectively;
these three targets have compile verification, not hardware OTA acceptance here.

Earlier ESP32-C3 hardware checks passed API/settings, concurrent SSE and page loads, rejection of
a second observer, physical key rotation with an active observer, and native OTA
A→B→A. Both OTA images were 961,968 bytes. Each update, reboot and health-confirmation
phase took about 23 seconds. Invalid SHA-256 and an interrupted 8192-byte upload
left the running slot and uptime unchanged; subsequent good updates succeeded.
Settings and the same key survived both updates. Five page loads alongside SSE
left a measured minimum free heap of 156,532 bytes; final idle free heap was about
169,000 bytes (reported bytes vary with the current connection state).

These earlier C3 results do not prove hardware acceptance on another MCU. At
that checkpoint the shared S3 16 MiB profile was build-only; the later explicitly
authorized S3 HTTP/SSE check is recorded above. Original NVS/PHY/project bounds
remain the preservation contract.

The final Pico image also passed native A→B→A hardware acceptance: both complete
uploads returned 202 in 9.5–9.8 seconds and booted the opposite slot. The existing
key and settings survived both boots. Interrupted input and a complete image with
the wrong digest left the running image active. Its SSE application payload is
616,704 bytes; the matched WS comparison is 631,040 bytes, so SSE saves 14,336 bytes.
The observed FreeRTOS free heap was about 124,424 bytes, with a 124,336-byte minimum.
Earlier missing-network diagnostics and the separate slow upload are recorded
in the [Pico guide](rp/README.md); they are distinct from this final acceptance.
Pico's reported heap is the FreeRTOS heap, not the separate C++/lwIP memory pools.
Windows native checks pass. The available Zig 0.14.1 Linux cross-toolchain fails
to compile floating-point `std::from_chars` in the existing JSON module; this
toolchain is therefore not a validated compact Linux profile.

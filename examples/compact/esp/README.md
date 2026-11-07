# Compact ESP demo

One native NaWServer instance, one persistent API key and two SSE observers. The
browser interface opens directly without a login or API key. The
page is packed into the firmware; no Vue bundle or filesystem mount is required.
TLS is not enabled. The `naw_compact` NVS namespace is separate from the full demo.

## Resource telemetry and comparison

With PSRAM disabled, reported heap values use `MALLOC_CAP_8BIT`. With PSRAM
enabled, the primary heap uses `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` and the
secondary heap uses `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`, so internal SRAM is
not conflated with external memory. `total_heap` is the total registered primary
capacity; free, minimum and largest block come from one `heap_caps_get_info`
call. The total includes allocator overhead; subtracting free from total measures
occupied allocator capacity rather than application payload alone. The minimum
is the sum of each region's historical low watermark,
which need not have occurred at the same instant. A single allocation is limited
by the largest free block, even when the sum of free bytes is larger.

The UI's free percentage uses `100 * free_heap / total_heap`. This is an
allocator-capacity percentage, separate from physical SRAM. For example,
[ESP32-C3 has 400 KiB SRAM, including 16 KiB cache, plus 8 KiB RTC SRAM](https://documentation.espressif.com/esp32-c3_datasheet_en.html).
Firmware data, instructions, cache and reserved regions explain why the runtime
heap total is smaller. Physical capacity is board/chip metadata and must not be
substituted as the denominator for a heap percentage. The native capability API
is documented in the [ESP-IDF heap reference](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-reference/system/mem_alloc.html).

CPU load uses the change in each configured core's native FreeRTOS idle-runtime
counter over elapsed ESP-timer microseconds. The reported value is the average
busy percentage across those cores. The sampler runs from `poll` at most once
per `NAWSERVER_DEMO_CPU_SAMPLE_INTERVAL_MS` (default 1000), allocates nothing and
does not enumerate tasks. `status` copies the latest result under a short mutex;
it never forces another CPU sample. Before the first complete interval, or when
the required runtime support is unavailable, `cpu_available` is false. The
default SDK configuration enables ESP-timer runtime accounting with 64-bit
counters. `NAWSERVER_DEMO_CPU_STATS=0` removes the demo sampler; disable native
`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` too when its instrumentation is not
wanted elsewhere. This is RTOS idle accounting, not a cycle-level ISR profiler.
The cumulative `cpu_busy_us` and `cpu_sample_us` fields count occupied and total
core-microseconds over complete sampling intervals. Their boundary differences
give a window mean: `100 * delta(cpu_busy_us) / delta(cpu_sample_us)`. Reading
`/ui/metrics` only before and after an idle window therefore avoids continuous
HTTP observer traffic. Sampling intervals containing those two boundary requests
remain part of the result; the counters are not an interrupt-time profiler.

For a comparison, use matching optimization settings and a stable network; keep
API keys and settings intact. Record the firmware identity, heap domain, stream
period, core count, sample count and observation duration with each result:

| Scenario | Observation | Report |
|---|---|---|
| Idle | Close all pages/streams, settle 30 s, read metrics, wait 60 s, read metrics again | Mean CPU from cumulative deltas; boundary free heap; boot minimum separately |
| Open page | One public SSE page for 60 s at the same configured interval | CPU mean/p95/max; free heap minimum; largest block minimum |
| OTA | Keep one observer while one native streamed update runs | Upload duration, observed CPU/memory extrema, reconnect and persistent settings |

A pair of idle snapshots supplies a mean, without reconstructing the whole
minute's CPU distribution. Idle CPU percentiles would require a bounded on-device
summary recorder; polling every second instead measures idle plus HTTP telemetry. OTA observations
end at reboot; report post-boot values separately. A boot-lifetime minimum is not
a scenario-local memory minimum. Do not overwrite the large demo merely to obtain
a historical baseline without separate authorization.

## Build

Run from the NaWServer repository root with the already installed ESP-IDF 5.5.6
PlatformIO environment, matching GCC14.2 toolchains, CMake, Ninja, Node.js
asset packer and SDK Python packages. The script does not install dependencies.
It selects the installed SDK Python interpreter explicitly; its dependency check
was already satisfied by this pinned PlatformIO environment (which does not keep
Espressif's external constraints file).

```powershell
./examples/compact/esp/Build.ps1 -Target esp32c3 -WifiHeader path/to/wifi_secret.hpp
./examples/compact/esp/Build.ps1 -Target esp32c6
./examples/compact/esp/Build.ps1 -Target esp32
./examples/compact/esp/Build.ps1 -Target esp32s3
```

When adding runtime-statistics settings to an existing checkout, choose a fresh
absolute `-BuildDirectory`, for example
`<repo>/examples/compact/esp/.build/esp32c3-metrics`. ESP-IDF preserves
values from an existing `sdkconfig`; changed defaults do not turn an existing
disabled runtime-statistics setting on. Confirm the generated configuration has
`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`,
`CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y` and
`CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y` before using CPU results.

Build requires a Wi-Fi header on first use. The supplied header is copied into ignored `.private/wifi_secret.hpp` and defines
`NAWSERVER_DEMO_WIFI_SSID` and `NAWSERVER_DEMO_WIFI_PASSWORD`. Subsequent builds
reuse that local header. A missing source or private header stops before configuration. Other boards are compile targets; a successful build is
not evidence of hardware OTA support on an attached device.

Outputs are under `.build/<target>-sse/`. Use `-StatusProtocol ws` for the comparison
build in a separate `-ws` directory. Use `-Version 0.1.1-compact` for a distinguishable
OTA candidate; copy the previous `.bin` before rebuilding the same directory when
both images are needed for an A→B→A test. All platform/core translation units use
`NAWSERVER_API_KEYS_MAX=1` consistently.

The SDK memory candidates are opt-in and have isolated build directories:

```powershell
./examples/compact/esp/Build.ps1 -Target esp32c3 -NAWSERVER_DEMO_MEMORY_PROFILE lowmem
./examples/compact/esp/Build.ps1 -Target esp32c3 -NAWSERVER_DEMO_MEMORY_PROFILE tight
```

`baseline` keeps the SDK defaults. `lowmem` and `tight` reduce Wi-Fi buffer
counts, lwIP socket count, TCP windows and the TCP/IP mailbox. Their actual
configuration is visible in `sdkconfig.lowmem.defaults` and
`sdkconfig.tight.defaults`. They are experiments until the requested concurrent
API / two SSE page / transfer and stress scenarios pass on the target hardware.
The script refuses an existing custom build cache that does not match the named
candidate, since ESP-IDF otherwise silently retains older sdkconfig values.
`-NAWSERVER_DEMO_TX_MEMORY_MODE 0`, `1` or `2` separately selects lazy TX
allocation, a preallocated heap arena, or a static BSS arena. The default is0;
nonzero variants also receive separate build directories. These are independent
of the Wi-Fi/lwIP memory profile and require the same hardware acceptance checks.
`-NAWSERVER_DEMO_TX_BLOCKS` selects 2–12 blocks (default4), and
`-NAWSERVER_DEMO_TX_BLOCK_BYTES` selects aligned block size (default1024,
minimum768, multiple of16). A block must fit a complete SSE frame; non-default
values also get separate build folders.
`-NAWSERVER_DEMO_HTTP_JOBS` and `-NAWSERVER_DEMO_CONNECTIONS` select the
concurrent HTTP workspaces and connection limit (defaults 2 and 4); two
connections must remain available for SSE. More workspaces cost memory but
permit extra requests while OTA and another API request are active.
For the identified S3 with 8 MiB octal PSRAM only, `-NAWSERVER_DEMO_PSRAM on`
builds a separate candidate that permits malloc and selected Wi-Fi/lwIP buffers
to use external RAM. A 32 KiB internal-only reserve remains for DMA and other
internal-capability allocations. This profile trades external RAM for internal
headroom; it is not evidence of lower total memory consumption.
ESP-IDF selects static Wi-Fi TX buffers when PSRAM is enabled, so its Wi-Fi TX
allocation policy differs from the non-PSRAM low-memory configuration.

## Initial C3 installation and retained data

Identify the target chip/MAC/port before writing. The following describes only the
C3 4MiB profile; never apply these offsets to another board by assumption.

| Region | Address and size | Initial installation |
|---|---|---|
| Bootloader | `0x0000`, generated image | Replaced to enable native rollback |
| Partition table | `0x8000`, generated image | Splits only the former application range |
| Existing NVS | `0x9000`, `0x5000` | Preserved; new namespace only |
| OTA selection | `0xE000`, `0x2000` | Initialized for the new two-slot layout |
| OTA0 | `0x10000`, `0x140000` | Initial compact application |
| OTA1 | `0x150000`, `0x140000` | Subsequent OTA target |
| Existing EEPROM | `0x290000`, `0x1000` | Preserved |
| Existing LittleFS | `0x291000`, `0x150000` | Preserved; not mounted or formatted |

Use the SDK-generated `flash_args` from the target build directory for this
**initial installation**. For C3 it lists exactly bootloader at0, application
at0x10000, partition table at0x8000 and `ota_data_initial.bin` at0xE000. It contains
no NVS/EEPROM/filesystem image. Do not run chip erase or add data partition images.

```powershell
# Inside examples/compact/esp/.build/esp32c3-sse, activated SDK environment:
python -m esptool --chip esp32c3 --port COM_PORT --baud 921600 write-flash '@flash_args'
```

Use the real identified port in place of `COM_PORT`. Built-in vendor programming
checks are sufficient; no additional full-flash readback/verify pass is needed.
Repeating initial `flash_args` resets OTA selection; routine updates instead use
`POST /api/firmware` and never rewrite the bootloader/table/NVS/filesystem.

## Provisioning and OTA

The first successful NVS initialization creates and prints `COMPACT_KEY=...` once
on the physical console after RF starts. Keep the credential private. Sending
physical `k` explicitly rotates it, persists the replacement and prints only that
new key. The browser page and `/ui/*` routes are public and require no key.
External `/api/*` clients send `X-Client-Key`; missing or invalid credentials are
rejected there. Keys are never placed in a URL.

The page's explicit key-issuance action calls `POST /ui/api-key`. The platform
creates or rotates slot0 under the same mutex used by physical `k`, persists it,
and returns201 with the new secret once. Later status/list responses do not
reveal the stored secret. This web action is available without a login, matching
the selected public web-interface policy. The external API retains its key check.

Upload an application `.bin` as `application/octet-stream`, with Content-Length
and `X-Firmware-SHA256` containing the64hex-digit SHA-256 of the complete file.
The inactive partition is selected natively; the firmware is read in1024-byte
blocks, hashed incrementally and written with sequential sector erasure. Full
image validation and hash comparison precede boot selection. Aborted or rejected
uploads retain the current boot selection. The other partition can be overwritten
by an attempted upload, so the currently running image is the recovery image.

After a successful HTTP reply, reboot is delayed by1second. The pending image is
confirmed only after the server has run for5seconds; a reboot before confirmation
allows the rollback-enabled SDK bootloader to select the previous valid image.
Wait for confirmation before starting another update. Maximum upload size is the
inactive slot size (`0x140000` in this profile). Body buffers are released on finish
and abort; SDK OTA/hash state does not retain the uploaded image in RAM.

An A→B→A hardware test and negative hash/truncation tests are required before
claiming that OTA has been verified on a particular MCU. Build results alone do
not establish those properties.

## Shared S3 16 MiB profile

`-Target esp32s3` appends `sdkconfig.s3.defaults` and selects
`partitions.s3.csv`, independently of the C3 4 MiB layout. A stale build cache
with another Flash profile is rejected: choose a fresh `-BuildDirectory`.
This profile is specifically for the shared 16 MiB S3 whose original table has
NVS at0x9000/0x6000, PHY at0xF000/0x1000, factory application at0x10000/0x500000,
and shared project storage at0xC00000/0x400000. It is not a generic S3 layout.
The demo assigns 8192 bytes each to the HTTP and main tasks after smaller
stacks overflowed on the tested C3 and S3. For a
diagnostic build, add `-NAWSERVER_DEMO_STACK_DIAGNOSTICS` to print task stack
low-water marks to the physical console every five seconds. Leave it disabled
for ordinary memory and CPU measurements.
`-NAWSERVER_DEMO_MAIN_STACK 6144` builds an isolated lower-stack candidate;
the default remains 8192 and a lower setting must pass the target's OTA,
configuration and concurrent-client stack-watermark checks.

| Region | Offset | Size | Relationship to the shared board |
|---|---|---|---|
| NVS | `0x9000` | `0x6000` | Original bounds retained |
| PHY | `0xF000` | `0x1000` | Original bounds retained |
| OTA0 | `0x10000` | `0x500000` | Same application extent as original factory |
| OTA selection | `0x510000` | `0x2000` | Previously unused gap after application |
| OTA1 | `0x520000` | `0x500000` | Previously unused space, ends at0xA20000 |
| Project storage | `0xC00000` | `0x400000` | Original final4MiB retained |

Building the OTA table does not prove existing Flash contents outside named
partitions are disposable. The owner explicitly authorized the 2026-11-03 S3
installation; NVS and the shared project filesystem remain preserved. Never
erase the whole chip or write images to the retained data partitions.
Other ESP32/S3/C6 boards require their actual Flash capacity and retained-data
layout to be checked before installing an A/B table. Processor model alone does
not establish a4MiB or16MiB memory profile.

The fresh S3 metrics profile was compiled as `0.1.3-compact`: the application is
894000 bytes (`0xDA430`). The generated configuration selects two cores,
performance optimization, ESP-timer runtime statistics with 64-bit counters,
16 MiB Flash and this S3-specific partition table. The generated partition binary
retains the NVS, PHY and shared project bounds shown above. This verifies the
two-core sampler compiles; it does not establish measured S3 CPU load, available
RAM or OTA operation. No S3 firmware, bootloader, partition table or data was
written during this check.

## Verification snapshot (2026-11-03)

Final ESP-IDF 5.5.6 builds with the public browser UI, version `0.1.2-compact`:

| Target / protocol | Firmware bytes |
|---|---:|
| ESP32-C3 / SSE | 963904 |
| ESP32-C3 / WS comparison | 973920 |
| ESP32-C6 / SSE | 1016048 |
| ESP32 / SSE | 893824 |

The matched C3 build saves 10016 bytes of Flash with SSE. This comparison excludes
runtime worker/stream allocations; ELF section totals containing linker dummy
sections must not be presented as physical RAM consumption.

Hardware acceptance on C3 completed `0.1.0-compact` in `ota0` → `0.1.1-compact`
in `ota1` → `0.1.0-compact` in `ota0`, using 961968-byte images. Each whole phase,
including reboot, network recovery and the boot confirmation wait, took about
23 seconds. The saved name, interval, LED setting and the same API key survived
both updates. A full image with an incorrect SHA-256 and an 8192-byte truncated
request were rejected without reboot or boot-slot change. The digest failure
closed the connection; it did not return a positive HTTP acknowledgement.

Additional C3 checks: unauthorized API 401, valid configuration 200, one active
SSE with continuing HTML loads, second SSE 503, and physical key rotation with
old-key 401/new-key 200 and closure of the old observer. Five 10666-byte decoded
HTML responses took 120–145 ms while seven SSE events arrived. Free heap after
boot was approximately 169000 bytes; the observed minimum
with that HTTP/SSE scenario was 156532 bytes. These are short functional checks,
not a long-duration stress test or a measurement of other SDK/board profiles.

The final public-UI C3 image was then delivered by OTA as `0.1.2-compact` and
checked in an actual Chromium browser: the page starts without a login, its
API-key field is readonly and initially empty, and its explicit creation button
returns201. The new key authorizes the external API (200), the old key returns401,
and the existing public SSE stream continues without reconnecting. A screenshot
was captured with the key field empty. A second public OTA of the same image
switched back to the other slot; both settings and the newly issued key survived.
The two final OTA phases took22.4 and22.7 seconds including network recovery and
boot confirmation. Observed final-profile free heap was approximately168000
bytes, with a160768-byte minimum during the short browser run. These final
hardware checks include the pending-reboot telemetry-drain guard. ESP32/C6
results above are compile verification; S3 was not flashed by this workflow.

To repeat explicit hardware OTA acceptance with private credentials:

```sh
node examples/compact/tests/esp-ota-accept.mjs http://DEVICE KEY_FILE A.bin B.bin REPORT_FILE
```

Before the test, save nondefault settings through the page or API so persistence
checks cannot succeed merely by returning defaults. The harness compares those
existing settings across both boots.

The command writes the inactive firmware slot and performs both OTA reboots;
provide distinct A/B versions and preserve the intended A image before building B.
The key file contains only the secret. The harness never places it in a URL or
report. Keep credentials, board identifiers and generated reports outside source
control, for example in the demo's ignored `.private/` and `.local/` directories.

## Memory and TX servicing experiments

`-NAWSERVER_DEMO_TX_EXECUTOR 0` keeps the dedicated TX worker (default).
`-NAWSERVER_DEMO_TX_EXECUTOR 1` services the same bounded queue through existing
HTTP/SSE/WS I/O callers. Queue limits and payload placement are independent.
`/ui/metrics` reports `tx_execution`; this choice does not add another queue.

For startup attribution use `-NAWSERVER_DEMO_MEMORY_BREAKDOWN`; add
`-NAWSERVER_DEMO_MEMORY_TRACE` for SDK allocation call stacks. Use fresh build
directories when changing tracing: frame pointers, trace code and its 22528-byte
static record buffer change the image and available memory. Trace allocation
turnover is not peak live usage. The minimum-monitor bookkeeping is balanced in
both phase snapshots; per-region heap minima are a conservative envelope.

`-NAWSERVER_DEMO_MEMORY_LINK_PROFILE 1..4` is **link-only**: network, native HTTPD,
plain NaW HTTP, managed NaW + TX. These images have no OTA and must not be flashed.
Profile `0` restores the regular demo. Compare DRAM/BSS/IRAM separately from the
runtime heap: static and dynamic ownership of façade objects differ between the
link-only and runtime harnesses. See `../MEMORY_BRICKS_RU.md` for measurements.

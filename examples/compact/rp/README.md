# Compact Pico 2 W

Native C++17, Pico SDK 2.3.1, FreeRTOS and the common NaWServer TCP engine.
The compact profile uses one CPU core. The second core stays in ROM, allowing
bounded IRQ-only flash-safe operations without a flash lockout task.

## Build

Use the installed Arm compiler, Ninja, `picotool` and `pioasm`; nothing downloads
SDKs or copies dependency trees. Set the following CMake cache paths explicitly:

```
PICO_SDK_PATH
PICO_TOOLCHAIN_PATH
picotool_DIR
pioasm_DIR
FREERTOS_KERNEL_PATH
NAWSERVER_DEMO_LITTLEFS_PATH
```

The default FreeRTOS/LittleFS locations reuse the existing RP demo's dependency
checkout. They are configurable. Copy `wifi_config.h.example` to
`.private/wifi_config.h` and fill the local network credentials. Then configure
this directory with Ninja and `CMAKE_BUILD_TYPE=Release`, and build normally.
The executable is `nawserver_compact_rp`.
Status uses SSE by default. Set `NAWSERVER_DEMO_STATUS_WS=ON` for the alternate
WebSocket profile; the unselected protocol is excluded from the library build.

RP memory pools are independently configurable at CMake configure time. Defaults
preserve the previously tested profile; reduced values need workload acceptance.

| CMake setting | Default | Allocation domain |
|---|---:|---|
| `NAWSERVER_DEMO_RP_RTOS_HEAP_BYTES` | 163840 | FreeRTOS task stacks and objects |
| `NAWSERVER_DEMO_RP_LWIP_HEAP_BYTES` | 32768 | lwIP allocator arena |
| `NAWSERVER_DEMO_RP_PBUF_POOL_SIZE` | 24 | packet buffers |
| `NAWSERVER_DEMO_RP_TCP_SEGMENTS` | 48 | queued TCP segment objects |
| `NAWSERVER_DEMO_RP_TCP_PCB_COUNT` | 12 | TCP connection objects |
| `NAWSERVER_DEMO_RP_NETCONN_COUNT` | 16 | socket/netconn objects |
| `NAWSERVER_DEMO_RP_TASK_DIAGNOSTICS` | OFF | optional USB task stack watermark snapshot |
| `NAWSERVER_PICO_HTTP_STACK_WORDS` | 3072 | HTTP worker stack in FreeRTOS words |
| `NAWSERVER_PICO_TX_STACK_WORDS` | 1024 | transmission worker stack in FreeRTOS words |
| `NAWSERVER_PICO_STREAM_STACK_WORDS` | 1536 | SSE/WS worker stack in FreeRTOS words |
| `NAWSERVER_DEMO_HTTP_JOBS` | 2 | maximum concurrent HTTP jobs |
| `NAWSERVER_DEMO_CONNECTIONS` | 4 | maximum concurrent accepted connections |

With task diagnostics enabled, USB command `r` includes the minimum free stack
for up to 24 current FreeRTOS tasks, in words with the word size also printed.
The snapshot reserves additional static RAM and is intended for a separate
diagnostic build; it is not a substitute for testing the production build.
Here one FreeRTOS stack word is 4 bytes. For example, configure an isolated
candidate with `-DNAWSERVER_PICO_HTTP_STACK_WORDS=2048`,
`-DNAWSERVER_PICO_TX_STACK_WORDS=768`, and
`-DNAWSERVER_PICO_STREAM_STACK_WORDS=1024`; the default depths remain more
conservative. Reducing these values releases space inside the RTOS heap; it
does not shrink that preallocated heap or the ELF's static RAM on its own.

Compare stack candidates using the same SDK, compiler, other pool sizes and
workload. Record each task's allocated depth and its lowest observed free
watermark, in bytes, during normal operation, simultaneous clients, OTA and
recovery. The lowest observed watermark only bounds the tested execution; keep
headroom for untested paths and interrupts. Separately record the normal build's
RTOS and newlib free minima, fixed/static RAM and TX allocation peaks. Do not
compare a diagnostic build's free RAM directly against a normal build without
including the snapshot array's static cost. OTA reboots reset the task watermarks,
so request the snapshot before the restart when measuring the upload path.
The connection limit must cover the configured HTTP jobs plus two SSE streams
and cannot exceed the lwIP netconn pool. Increasing both HTTP jobs and
connections changes the memory and load profile and needs its own testing.

`NAWSERVER_DEMO_TX_MEMORY_MODE` selects lazy heap (`0`), startup-reserved heap
(`1`), or caller-owned static storage (`2`). `NAWSERVER_DEMO_TX_BLOCKS` is 2..12
(default 4); `NAWSERVER_DEMO_TX_BLOCK_BYTES` is 768..4096 and a multiple of 16
(default 1024). A block must hold one complete bounded SSE frame.

TX metadata placement is independent: `NAWSERVER_DEMO_TX_METADATA_MODE=0`
reserves an exact bounded table from the allocator at startup; `=1` uses the
demo's aligned caller-owned static arena, whose size is set by
`NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES` (default 2048). An undersized arena
causes startup failure rather than falling back to heap. The demo's one arena
is intended for one active backend; separate library users can provide their
own allocator/arena per instance. `/ui/metrics` reports requested metadata
bytes and actual job/connection capacities, not allocator bookkeeping or the
unused tail of a static arena. Static reservation moves bytes out of the newlib
range and must be counted even when no server is active.

For example, add `-DNAWSERVER_DEMO_RP_RTOS_HEAP_BYTES=98304` to an isolated
build directory. Values are compile definitions shared by the SDK, kernel and
application, so do not mix objects built with differing values. A smaller RTOS
heap also enlarges the distinct linker-controlled newlib arena; compare actual
free capacity of both allocators rather than just the FreeRTOS percentage.
`NAWSERVER_DEMO_UPLOAD_TIMEOUT_MS` defaults to 600000 on RP. This bounds the
whole update separately from the 30-second receive inactivity limit and
5-second header limit. The USB-powered demo disables Wi-Fi power saving while
preserving the SDK's beacon/DTIM/association-listen parameters.

## Exact flash layout

| Physical offset | Bytes | Purpose |
|---|---:|---|
| `0x000000` | 4096 | RP2350 ROM partition table |
| `0x001000` | 1044480 | Application A |
| `0x100000` | 1044480 | Application B |
| `0x1ff000` | 4096 | Unused |
| `0x200000` | 1835008 | Existing LittleFS, unchanged |
| `0x3c0000` | 262144 | Existing NVS, unchanged, read-only in this table |

This layout requires a **new partition table on first installation**. An older
showcase firmware at physical offset zero is not compatible with this table.
Installation must therefore be reviewed and performed as table plus application,
not as an OTA update to the old demo. No code here installs the table, formats
LittleFS, seeds assets, erases NVS, or flashes a connected board automatically.

Reviewed first-install sequence, in BOOTSEL mode:

```
picotool partition create partitions.json build/partitions.uf2
picotool load build/partitions.uf2
picotool reboot -u
picotool load -x build/nawserver_compact_rp.ota.uf2
```

Use the **`.ota.uf2`** artifact, including for this initial application load.
Do not load the SDK's ordinary `.uf2`: it may include the RP2350-E10 absolute
guard block outside these application slots, conflicting with existing NVS.
The dedicated artifact contains only secure Arm RP2350 application blocks.
`check-image.mjs` enforces addresses, payload sizes, numbering, slot capacity,
the compact Pico 2 W marker and TBYB image metadata. It emits a SHA-256 sidecar.

## Runtime update

The common `/api/firmware` endpoint receives the `.ota.uf2` bytes directly, with
the API key and `X-Firmware-SHA256` header. Browser hashing uses browser memory;
the browser uses the public `/ui/firmware` route without a login. API keys are
issued through the public UI or USB for external automation clients.
The board receives at most 1024 bytes per callback. The parser requires ordered,
contiguous 256-byte payloads and rejects other families, absolute addresses,
extension blocks, duplicates, changed counts and storage addresses.

The boot ROM selects the inactive A/B slot. The adapter checks this against the
currently booted partition and exact installed table before writing. The
candidate's first sector remains erased while its 4096 bytes are held in RAM.
Other pages are programmed and read back. Only after the complete upload,
SHA-256 and compact/TBYB metadata validation is that first sector programmed.
Its pages at offsets 256..4095 are programmed and checked before page zero.
Sector publication is not atomic; the ROM image hash rejects an incomplete
image after a power loss during this final phase.
Short/canceled/invalid uploads never request an update reboot.

After the successful HTTP reply, a 1500 ms application timer requests the ROM's
flash-update reboot. Boot ROM performs its own image validation. The candidate
is confirmed with `rom_explicit_buy` only after filesystem/key initialization,
native network stack initialization, web-server startup and acceptance of the
first asynchronous station connection attempt. The listener binds `0.0.0.0`
before association/DHCP; a slow or absent access point does not invalidate an
otherwise healthy firmware. Without confirmation, the ROM's TBYB
watchdog rolls back; application startup deliberately does not disable it.
It also does not call `watchdog_update` or replace the ROM watchdog configuration.
The image hash checks integrity; this demo does not provision OTP secure boot
or a signing key.

OTA state uses approximately 9 KiB of fixed RAM: first sector, ROM workspace,
one UF2 frame, SHA state and counters. Page readback adds 256 bytes of bounded
stack storage. It never holds the full firmware in RAM. Raw `.bin`, combined
UF2 packages and out-of-order UF2 files are intentionally not accepted.

Only `/web.compact-keys.0/.1` and `/web.compact-settings.0/.1` are written in
LittleFS. Existing files remain untouched. Mount failure stops startup; there
is no formatting fallback. A new key is shown once on USB when an empty compact
store is initialized. USB `k` explicitly rotates/reveals that compact key;
USB `b` enters BOOTSEL; `r` requests link/IP/server/FreeRTOS heap status from the
application task. Failed Wi-Fi attempts alternate WPA2/WPA3 after a 30-second
deadline, with a 3-second retry interval. No credentials appear in status output.
USB `s` performs a bounded scan only while disconnected, reporting counts and
the configured network's channel/RSSI without printing network names or MACs.
Key authentication and physical rotation share a mutex.
LittleFS operations, OTA flash operations and native image confirmation share
one flash coordinator; it never calls back into filesystem/application code.

Native reference: [Raspberry Pi OTA example](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi/ota_update).
This implementation uses the installed SDK's public Boot ROM API and does not
copy private Boot ROM offsets used inside SDK helpers.

## Validation and current limits

The UF2/metadata parser has a standalone host test in `tests`. The reproducible
hardware runner `tests/hardware_ota.mjs` checks interrupted uploads, a wrong
SHA-256, alternating slots and retained settings/key access. It requires an
explicit host, image, private key file and report path; it is not run by builds.

On 2026-11-03 the final public-UI image passed the complete hardware suite:
1,233,408 wire bytes, SHA-256
`85aed145530abb1452ba9c9172f58c738a5821c85d05831f28828fd1483fd17c`.
Interrupted upload and full-image wrong SHA-256 did not activate the candidate.
The wrong-hash transfer ended after 9.420 seconds. Valid updates replied HTTP
202 after 9.549 seconds (A to B) and 9.782 seconds (B to A); both rebooted into
the expected slot. The existing API key remained valid and the existing name,
interval and LED setting survived both boots. The runner neither rotates the
key nor changes those settings. Final RSSI was -61 dBm.

An earlier image took 114.4 seconds on a poor connection, before diagnostic
changes. Subsequently, a RAM-only SDK scan without FreeRTOS, TCP or flash writes
saw other access points but no configured-network match. The complete passing
suite above ran after network availability returned. Both radio conditions and
firmware changed, so these timings do not isolate a single cause of improvement.

Measured flash operations during upload: maximum erase 36.5 ms, program 0.58 ms,
read 0.13 ms; approximately 0.256 seconds total flash critical-section time in
the first five seconds. Upload callbacks yield one RTOS tick after an erase and
after each bounded input block. They emit no USB log output while transferring.
USB `r` exposes accumulated flash timing after the transfer.

The final SSE public-UI build was 616,704 bytes of application payload, with
269,068 bytes of ELF BSS. The alternate WS build was 631,040 bytes of payload;
SSE saved 14 KiB. The WS comparison was build-only, not a hardware acceptance run.
Observed FreeRTOS free heap was 124,424 bytes (about 121.5 KiB) out of 160 KiB,
with a 124,336-byte low-water mark after both updates. This counter does not
include the separate C/C++ allocator. It is not a total free-RAM measurement.
See `tests/radio_scan/README.md` for the RAM-only diagnostic.

## Runtime resource measurements

For a startup allocation experiment, set `NAWSERVER_DEMO_RP_STARTUP_MEASURE=ON`
in a separate build directory. The diagnostic trial boot starts the server
before confirming the image; after successful confirmation and Wi-Fi connection
it performs one controlled reboot. On the confirmed boot it attempts Wi-Fi
association before constructing the transport, backend and demo and retains
seven numbered snapshots. `wifi_ready=1` and `pending=0` are required for a
connected-Wi-Fi comparison. The ROM trial watchdog is not disabled or extended.

After the startup snapshots, a temporary diagnostic listener opens on TCP port
81 for at most 60 seconds. Its first request receives the snapshots; when no
SSE/WS client and no queued, active or filling TX job exists, the probe also
stops and restarts the demo and returns three corresponding snapshots. The
port-81 connection itself is already allocated in those stop/restart snapshots,
but was not present in the startup snapshots. An opt-in
`NAWSERVER_DEMO_RP_PROBE_CYCLES` (1..5, default 1) permits repeated diagnostic
cycles; the listener closes after the configured number or timeout. Only use
the diagnostic build in a controlled network. The `r` USB command also prints
the startup samples, and `m` requests a stop/restart. These diagnostic arrays,
stack use and temporary socket are additional costs; compare linker maps.
Leave the option OFF for the usual early-listener startup policy.

`NAWSERVER_DEMO_RP_NO_WEB_BASELINE=ON` is a **build-only** reference without
the transport, backend or demo, for comparing linker `end` and static RAM. Its
runtime is not an acceptance target and it does not produce the dedicated OTA
artifact. Differences between separately linked ELFs include linker layout and
garbage-collection effects; they are not a direct measurement of heap allocation.

The platform samples resource counters once per second from its existing
application task, including when nobody has opened a page. HTTP and SSE read
the cached snapshot; they do not enumerate tasks or allocate a measurement task.
CPU utilization is `100 * (wall_delta - idle_delta) / wall_delta`, where both
values use the Pico hardware microsecond timer through FreeRTOS runtime stats.
This avoids using SysTick as elapsed time when flash operations mask interrupts.
The result describes scheduler utilization on CPU0. Interrupt time is charged
to the interrupted task by FreeRTOS, so this is not separate ISR accounting.
One core is active; CPU1 stays in ROM and is not included in the denominator.

The main heap counters describe the configured **160 KiB FreeRTOS heap**.
Its free, minimum-free and largest-free-block values come from `vPortGetHeapStats`.
This heap holds task stacks and RTOS objects; it is neither all 520 KiB of
physical SRAM nor the allocator used by C++ `new`. Static lwIP arenas and the
newlib/C++ heap are distinct, and must not be silently combined with this heap.

The secondary `newlib/C++` heap covers the linker range `end..__StackLimit`.
Free space is that capacity minus `mallinfo().uordblks`, including unused
allocator chunks and capacity not yet committed by `sbrk`. Allocator metadata
belongs to its used bytes. Standard newlib recursive allocation hooks serialize
this snapshot with libc allocations; they supplement Pico's outer malloc lock.
The SDK's default newlib retarget lock functions are empty in the tested toolchain.
No largest-free-block value is claimed for this secondary allocator.

`static_ram_bytes` describes the linked main-RAM region below `end`, **including**
the entire FreeRTOS heap and static lwIP pools. It cannot be added to their
capacities. `physical_ram_bytes` is 532,480 bytes, including the 8 KiB scratch
banks holding the core/interrupt stacks. Static occupancy and fixed reservations
do not measure live allocation within lwIP; lwIP statistics remain disabled.
The cumulative `cpu_busy_us` / `cpu_sample_us` counters permit two boundary
reads around a quiet interval without a continuous polling load.

# RP2350 compatibility findings — demo-local experiment

## Update after shared-library changes were authorized, 2026-11-02

The following notes describe the initial demo-only port. They are historical,
not the current build instructions. Shared lwIP and FreeRTOS workers, an internal
Mutex boundary, portable CMake targets, bounded source responses and platform UI
profiles now live in the library/shared frontend. The 80 KiB whole-file buffer,
copied CPU panel and build-time platform substitutions have been removed.
See [portable builds](../../docs/readme_portable_build_ru.md) and
[unified transmission](../../docs/readme_unified_transmission_ru.md).
The tested GNU shared_ptr thread-model bridge remains required; it has not been
replaced with a complete standard C++ threading implementation. CPU0/libc and
flash-safety constraints below still apply. The endpoint allows four WS clients
and ten HTTP/WS/SSE connections; source files use bounded queue blocks.

## Initial demo-only findings

Owner boundary: NaWServer core, public headers, shared frontend, other examples,
and root build files are frozen for this work. All adaptations below live in this
directory. This is an experimental RP demo, not a claim of a released RP backend.

| Finding / source evidence | Demo treatment | Candidate library work, deferred |
| --- | --- | --- |
| Root `CMakeLists.txt` is an ESP-IDF component; host CMake is OS-specific. | Explicitly compile canonical core/protocol source files in this demo's CMake. | Export a standalone, SDK-independent core target with explicit platform source selection. |
| TCP, buffers, WS and SSE use `std::mutex`; bare Arm GCC libstdc++ has no gthread OS port. The first build failed before a usable mutex port was supplied. | `compat/bits/gthr.h` supplies a narrow FreeRTOS-backed GNU mutex implementation. Standard C++ classes are not replaced and toolchain files are untouched. Unsupported condition-variable/once functions remain deliberately undefined. | Decide the supported RTOS/toolchain mutex contract or introduce a narrow internal synchronization boundary. Do not make the core own FreeRTOS configuration. |
| TCP private `platform/worker.h` needs an execution implementation. Existing MCU worker uses `esp_timer`, IDF byte stack sizes, IDF TLS/socket cleanup and SMP queries. | `src/rtos_worker.cpp`: upstream FreeRTOS stack **words**, workers/join callers pinned to CPU0, explicit task-local worker identity, bounded join. | Add a reviewed RP worker implementation and build selection after the owner lifts the source freeze. |
| Existing concrete transport is named `IdfTransport`; its lwIP policy depends on `CONFIG_LWIP_MAX_SOCKETS`. | Demo-local `PicoTransport`, derived from the existing MIT adapter, uses the Pico lwIP configuration and keeps handle ownership/retained-close semantics. | Review a standalone lwIP adapter or separate the SDK-specific policy from shared socket operations. |
| Pico lwIP default BSD alias macros collide with method names such as `accept`/`close`; private `timeval` conflicts with newlib. These are observed compile errors. | Set `LWIP_COMPAT_SOCKETS=0`, `LWIP_TIMEVAL_PRIVATE=0`, use explicit `lwip_*` calls. | Port documentation/build configuration; no HTTP semantic change is needed. |
| Pico SDK FreeRTOS integration requires timer-pended calls and mutex-holder queries. | Explicit FreeRTOSConfig.h switches; official RP2350 ARM secure-only port with MPU/TrustZone disabled, two scheduled cores with explicit idle/timer affinity. | Document an exact supported kernel/port/SDK matrix when RP becomes a library target. |
| Earlier bare-metal Wi-Fi could stall in the SDK semaphore wait without driver logging. | The experimental helper is retained behind `NAWSERVER_DEMO_RP_BOUNDED_WIFI_WAIT`, **OFF by default**. The working FreeRTOS image uses the normal SDK wait implementation. No Pico SDK file is patched. | This belongs to board/SDK integration, not NaWServer core. |
| Public `ResponseTransport` accepts complete byte views/owned buffers, not a file reader. LittleFS files cannot generally be borrowed as one contiguous XIP span. | Shared Vue gzip assets fit a bounded 80 KiB serialized FS-read buffer; the packer rejects larger assets. | A bounded synchronous source/file-response contract is needed for large FS resources without whole-file RAM copies. Preserve existing lifetime and serialized-callback rules. |
| Shared Vue Vite configuration explicitly allows only showcase/touch_lcd7/c6_lcd147 modes. | Demo-local Vite config compiles the canonical shared frontend with a local project.ts, storage page and notification indicator. No component/style copies. | Generalize project selection and platform labels through the profile API in a later authorized change. |
| Pico has no ESP-IDF NVS service or IDF partition table runtime. | Application-owned fixed flash ranges, LittleFS and a separate CRC/generation/commit-page KV journal. | No storage mount or product settings belong in the server library. |

## Boundaries of current work

The library protocol/dispatcher/router/WS/SSE sources are compiled directly from
their canonical locations. No patched copies of core sources or alternative
HTTP implementation are introduced. Only the concrete transport is kept as a
demo-local experimental adapter. Native ESP-IDF, Windows and Linux builds are
not rebuilt as part of this owner-scoped RP demo.

Kernel V11.3.3: `89592801dd8b73329a7583719b7e3567b50910e5`.
Community-supported port: `bae4c7aa19009825ba48071a8fe25dcb8be84880`.
LittleFS v2.11.3: `6cb4e86540eca0d9ba62500a298385c9d863c8be`.
Pico SDK 2.3.1: `079c6f39023649b154152db30f1d781e884879bc`.

The NVS publication readback is part of the small transactional write algorithm;
it is not a separate firmware verify/readback pass. Firmware flashing omits `-v`.

The demo initially mismatched a 1024-byte WS transmit block with the endpoint's
4096-byte default. The existing library correctly rejected HTTP startup.
The full Vue UI now explicitly configures 4096-byte blocks/endpoint and two WS clients.
No library change is needed for that configuration error.

## Full ESP showcase UI on Pico

- The common CBOR v2 contract is implemented by demo-owned showcase_api.cpp. It
  reports actual FreeRTOS snapshots, microsecond runtime counters, heap_4 stats,
  Wi-Fi RSSI/MAC/IP, and hardware/storage identity. Summary updates use HTTP and
  binary WS; diagnostic SSE stays JSON as in the ESP demo.
- Pico SDK propagates `LWIP_PROVIDE_ERRNO=1`, while C++ uses newlib's per-task
  errno. That mismatch turned nonblocking EWOULDBLOCK into apparent fatal errno=0
  and truncated larger UI responses. Demo lwipopts.h undefines that option and
  selects `<errno.h>` consistently. No library patch or SDK modification.
- Gzip UI assets are bounded to 80 KiB per file, served from a serialized static
  buffer; the generated packer rejects larger files. The largest is about 73 KiB.
  A future portable streaming FS response would remove this whole-file SRAM cost.
- Shared UI still contains ESP labels and platform assumptions. A demo-only Vite
  transform changes those labels/active-core/workload limits at build time. A
  future profile API should own these labels instead of build substitutions.
- Shared notification toasts conflict with the owner's current UI standard. This
  demo substitutes a local fixed-size footer indicator, opening details only on
  click; core/shared frontend files remain frozen.
- CPU/task runtime is measured for both active cores; total is their average
  (0..100%), with raw sum (0..200%) in cpu_sum_pct. RAM telemetry covers the
  FreeRTOS heap, not the independent newlib heap.
  Transport counters cover the showcase API/WS; HTTP socket figures describe
  persistent WS/SSE occupancy, excluding transient synchronous HTTP requests.
- Current UI retains MCP navigation, but capability reports MCP unavailable;
  RGB, BLE runtime, simulator and connection listing are also explicitly disabled.

## Dual-core extension

- FreeRTOS SMP runs both ARM cores with explicit affinity. All SDK/libc/server
  tasks default to CPU0. The pure workload can run on either core. Task pages
  report actual affinity masks, and per-core utilization comes from the separate
  idle task runtime deltas; no second-core utilization is synthesized.
- The supplied newlib uses a global `_impure_ptr`. Demo configSET_TLS_BLOCK only
  changes it on CPU0. CPU1 runs no libc-dependent application work; moving general
  libc/SDK tasks there requires a true SMP libc integration, outside this demo.
- SDK's default per-page SMP flash helper triggered the RP FreeRTOS task-return
  assertion at port.c:870 during its short-lived task teardown. No dependency
  sources are patched. flash_guard.cpp supplies the SDK-supported helper callback:
  one permanent, statically allocated CPU1 task, running its flash-critical loop
  from SRAM with IRQs disabled. Storage stays serialized on CPU0.
- The shared UI already receives core_busy_pct and plots per-core task history.
  Demo-local CpuPanel.vue adds aligned CPU0/CPU1/total values and three traces on
  the INFO chart via the read-only build profile. Shared components remain frozen.
- Flash.ps1 detects exact-serial ROM USB presence through PnP rather than parsing
  application metadata; incomplete application metadata must not prevent recovery.
  Early startup guard returns failed initialization to ROM on watchdog restart.

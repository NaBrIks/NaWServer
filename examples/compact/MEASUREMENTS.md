# Compact resource measurements, 2026-11-03

These are short hardware measurements, not a long stress/soak acceptance. They
include the demo, RTOS and Wi-Fi stack; they do not isolate the library's cost.

## Current two-observer normal-load checks

These runs use two SSE connections plus one sequential API client and repeated
page downloads. They are API/wire tests, not two real-browser layout checks.
Both use lazy TX payload, four 1024-byte blocks, two HTTP work slots and four
physical connections. Neither row below includes an OTA phase.

| Profile | Window | Active cores | Mean CPU | API / page completions | Events per SSE observer | Largest SSE gap |
|---|---:|---:|---:|---:|---:|---:|
| S3 baseline, HTTP stack 8192 bytes | 30 s | 2 | 2.89% | 29 / 14 | 15 / 15 | 2023 ms |
| Pico 2 W, RTOS heap 160 KiB | 45 s | 1 | 4.54% | 43 / 21 | 22 / 22 | 2028 ms |

Both had zero API, page and SSE errors at the retained 2000 ms stream period.
S3 API/page maximum latencies were 92/215 ms; Pico's were 67/204 ms.

| Profile | Main heap sample minimum, bytes | SDK lifetime minimum, bytes | Largest main block sample minimum, bytes | C++ heap sample minimum, bytes |
|---|---:|---:|---:|---:|
| S3 baseline | 220024 | 212732 | 176128 | Included in main heap |
| Pico 2 W, RTOS heap 160 KiB | 123912 | 123704 | 123912 | 220080 |

Private evidence files are `s3-stack8192-smoke.json` and
`rp-baseline-smoke.json` in the ignored demo `.local` directory. S3's earlier
6144-byte HTTP-stack candidate overflowed; its failed startup/probe reports
are not included as passing runs. The 8192-byte result contains no task-stack
watermark, so it cannot establish the minimum safe stack. Profile variants,
PSRAM and TX storage modes require separate matching measurements; no complete
RAM/CPU ranking is inferred from the two rows.

## TX storage accounting for profile comparisons

The shared configuration `NAWSERVER_DEMO_TX_MEMORY_MODE` changes only the TX
payload backing. With `NAWSERVER_DEMO_TX_BLOCKS=4` and
`NAWSERVER_DEMO_TX_BLOCK_BYTES=1024`:

| Mode | Static application payload | Startup heap payload | On-demand heap payload |
|---|---:|---:|---|
| `0`, lazy heap | 0 bytes | 0 bytes | Actual-size leases within the 4096-byte TX ceiling |
| `1`, preallocated heap | 0 bytes | 4096 bytes | No payload growth beyond the fixed blocks |
| `2`, static RAM | 4096 bytes | 0 bytes | No payload growth beyond the supplied array |

The static array and preallocated heap blocks remain real RAM reservations
even when no client sends data. Lazy blocks release when their jobs complete
or cancel. Metadata, stacks, SDK network buffers and allocator overhead are
outside these payload columns and remain present in every mode. Static BSS
already belongs to `static_ram_bytes` where that field is implemented; do not
count it again as a separate physical RAM category.

The live `tx_bytes` includes both fixed and dynamic allocated payload;
`tx_reserved_bytes` is a subset of that value. `tx_peak_bytes` is a runtime
high-water mark. `tx_limit_bytes` is a maximum, not another allocation. The
separate 24 KiB TCP and 4096-byte SSE ceilings likewise are not startup
reservations; shared direct-TX SSE does not create a second frame-payload pool.
`tx_failures` combines queue refusals and failures, including retryable
backpressure. The passing normal S3/Pico runs ended with 145/228 on that counter
while all measured requests and both SSE streams succeeded, so it must not be
interpreted as a delivery-loss count.

## Earlier single-observer compact profile

The idle/page/OTA matrix below predates the two-observer diagnostic profile.
Keep these historical values separate from the current normal-load rows above.

| Device | Scenario | Mean active-core CPU | Main heap free, KiB | Main heap total, KiB | Free | C++ heap free, KiB |
|---|---|---:|---:|---:|---:|---:|
| C3 | Idle | 1.04% | 158.1 | 270.9 | 58.3% | Included in main heap |
| C3 | One real page | 1.58% | 157.4 | 270.9 | 58.1% | Included in main heap |
| C3 | OTA with that page open | 97.16% | 149.1 | 270.9 | 55.1% | Included in main heap |
| Pico 2 W | Idle | 2.02% | 121.6 | 160.0 | 76.0% | 215.8 / 240.5 |
| Pico 2 W | One real page | 2.33% | 121.4 | 160.0 | 75.9% | 215.7 / 240.5 |
| Pico 2 W | OTA with that page open | 74.17% | 121.3 | 160.0 | 75.8% | 214.7 / 240.5 |

Memory columns are the smallest observed free values in boundary/SSE snapshots,
not a guarantee that shorter allocation peaks were sampled. During C3 OTA the
SDK lifetime minimum fell from 154444 to **147552 bytes (144.1 KiB)**, below the
149.1 KiB sampled value. RP's FreeRTOS minimum did not fall below its pre-existing
124120-byte low-water mark in this run. The largest observed allocatable main
block was at least 114688 bytes on C3 and 124120 bytes on RP.

Idle/page windows were 20 seconds. CPU is `100 * delta(cpu_busy_us) /
delta(cpu_sample_us)`, measured from cumulative native idle counters, not an
average of rounded UI values. Samplers run once per second without task-array
allocation. There is one active core in both compact hardware profiles; RP CPU1
stays in ROM. These are scheduler utilization figures: IRQ time can be attributed
to the interrupted task, and a flash operation's non-idle wait also counts as busy.

The page was an actual headless Chromium instance running the deployed JavaScript.
Existing settings were retained: C3 SSE every 1000 ms, RP every 2000 ms. The page
windows received 20 and 10 records respectively, without background HTTP polling.
OTA transferred 968608 bytes on C3 in 12.621 seconds, and 1240576 UF2 wire bytes
on RP in 9.875 seconds. Status continued with 12 and 5 records during those uploads.
Both candidates booted the other slot with unchanged settings. No key was rotated.

The local catalog probes devices every 30 seconds. It was left running; the clean
20-second windows were placed between its probes. Detailed request/stream counters
confirmed one final measurement HTTP request during idle/page windows, zero idle
streams and no unexpected page-window requests. An initial rejected run contained
one extra catalog request and is not used as the clean idle result.

## Memory domains

C3 reports `MALLOC_CAP_8BIT` heap, including ordinary C++ allocations. Its 270.9 KiB
heap capacity is not the chip's entire SRAM. Espressif lists C3 physical memory
as 400 K in its [memory reference](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/system/ram-usage.html).

RP has 520 KiB of physical SRAM. The 160 KiB FreeRTOS allocator lives inside
static main RAM, while the separate C++/newlib arena is 246280 bytes. Static main
RAM is 278008 bytes and already includes the RTOS and lwIP reserved pools. Do not
add that static figure to both heaps. FreeRTOS plus C++ free memory at idle was
345488 bytes, about 337.4 KiB, or 64.9% of physical SRAM. This excludes unused space
inside reserved lwIP pools and does not mean one allocation can obtain that sum.
The C++ figures include unused allocator chunks plus uncommitted linker arena.
After the run, C3 received a metadata-only update to expose its 400 KiB physical
capacity in the same UI. That changed its BIN from968608 to968624 bytes; allocator
budgets and the sampling algorithm stayed unchanged. Physical capacity is not the
denominator of either heap percentage.

## Earlier large demos and native HTTPD

These records are historical measurements of different firmware, not a matched
current A/B run. Neither large demo has the compact firmware-upload endpoint.

| Recorded profile | Scenario | CPU | Free heap | Limitation |
|---|---|---:|---:|---|
| Large C3, earlier common TCP profile | Clean idle, 30 s | 1.68% | 118.1 KiB | Before the final key/admission profile |
| Same profile | One SSE at 1 Hz | 2.27% | 117.3 KiB | Controlled stream, not a real browser |
| Same profile | WS 10 Hz + SSE 1 Hz | 7.16% | 106.5 KiB | Simulates page channels |
| Latest large C3 with keys | One real page | Not recorded | Approximately 29–31 KiB | Different resource reservation profile |
| Earlier native HTTPD C3 | After browser/LAN SSE checks | Not recorded | 174.5 KiB | One snapshot, not separated idle/page/OTA |
| Large RP | Clean idle / one page | No valid matched result | Not comparable | Earlier candidate idle had external HTTP traffic |

Sources: [clean common-TCP observer report](../../docs/readme_20261103_clean_observer_measurements_ru.md),
[latest full key-demo acceptance](../../docs/readme_api_keys_ru.md), and
[native HTTPD SSE acceptance](../../docs/readme_sse_adapters_ru.md).
The latter recorded free178676/min155580/largest114688 bytes. It does not establish
the resource cost of a current HTTPD version with the same features as compact.
The old RP used two RTOS cores and a 192 KiB RTOS heap; compare neither its CPU
normalization nor its free-RTOS number directly to compact's single-core profile.

The linked build-only C3 comparison from 26 October, IDF 5.5.6, is also preserved in
the [build log](../../docs/readme_build_memory_log_ru.md): Minimal HTTPD/TCP BIN
964960/942464 bytes and data+BSS48340/48372 bytes; Showcase1114928/1092176 and
63540/63572 bytes. Those rows explicitly did not measure runtime heap or CPU.

An unchanged current large RP image cannot be installed through compact OTA: it
lacks the required partition/product/TBYB metadata and its ordinary UF2 includes
an absolute block outside the A/B slots. It also writes legacy NVM/assets at boot.
No partition migration or large-demo reinstall was performed for this comparison.

## Published third-party references

Checked 2026-11-03. These help establish scale; they are not a CPU ranking.

| Primary source | Published quantity | Scope and qualification |
|---|---|---|
| [Espressif memory comparison](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/system/ram-usage.html) | C3 station example about219K free; empty example332K | IDF release/v5.5; Wi-Fi baseline, no HTTP server |
| [PsychicHttp author](https://github.com/hoeken/PsychicHttp#https--ssl) | About150k free in a blank sketch; about45k per SSL connection | ESP32 HTTPD wrapper; board/profile not specified sufficiently for a C3 match |
| [ESP32Async configuration](https://esp32async.github.io/ESPAsyncWebServer/configuration/) | AsyncTCP stack defaults16k; author suggests4–8k for short callbacks | Configuration reservation, not total server RAM or a runtime benchmark |
| [ESP32-Task-Manager author](https://github.com/jameszah/ESP32-Task-Manager) | Reusing an HTTPD instance saves about10KB; monitor about2500B and below2% CPU | ESP32/ESP32-CAM; CPU figure describes the monitor, not the entire HTTP server |
| [Mongoose author footprint](https://mongoose.ws/articles/tcp-ip-footprint/) | HTTP+TCP/IP heap10020B at boot,11636B with one connection | STM32H723/Ethernet/bare-metal, excludes12KB DMA; not ESP32/RP/Wi-Fi |

The PsychicHttp author's [benchmark spreadsheet](https://github.com/hoeken/PsychicHttp/blob/master/benchmark/comparison.ods)
compares older PsychicHttp, ESPAsyncWebServer and ArduinoMongoose throughput and
latency. Its WS single-client results are38/71/62 exchanges per second respectively;
it does not supply comparable idle/page/OTA CPU percentages or heap measurements.
Historical failure claims about the old AsyncWebServer fork must not be applied
to the currently maintained ESP32Async versions. No comparable published
three-scenario CPU/heap matrix for C3 and Pico was found in this search.

## Reproduction

```sh
node examples/compact/tests/metrics-scenarios.mjs http://DEVICE IMAGE PRIVATE_REPORT 20
```

This explicitly writes firmware and reboots the selected board. Close external
device pages/clients first. Run boards sequentially. Reports, device identifiers,
private firmware and screenshots remain in ignored `.local/` directories. The
runner never creates/rotates API keys and never changes settings.
If a local development catalog is probing devices every30seconds, optionally set
`NAWSERVER_DEMO_MONITOR_URL` to its origin to align windows with its cached
`/api/links` probe timestamps. Without that setting the runner contacts no catalog;
its request counters still reject an idle/page window with extra traffic.

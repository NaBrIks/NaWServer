# Compact two-page resource load

`resource-load.mjs` exercises the deployed firmware with two concurrent SSE
observers, one API client authenticated using the existing key, and repeated
downloads of the device page. It never sends a complete settings update or
rotates a key. `--overload` additionally begins and disconnects partial JSON
requests to exercise an extra connection above the four-connection baseline.

```sh
node examples/compact/tests/resource-load.mjs BASE_ORIGIN PRIVATE_KEY_FILE PRIVATE_REPORT 300 --label=PROFILE
```

The key is read from a file; do not paste it into the command line. Put both the
key and report in ignored private locations. The report contains device status
and is not intended for source control. Duration is 10..86400 seconds. An
explicitly requested `--ota=FIRMWARE_FILE` **writes the inactive firmware slot
and reboots the device** after the load phase. During that upload both SSE
connections and the API client continue concurrently. Page downloads also
continue in the explicit `--overload` mode. The
runner checks the boot slot, unchanged settings and the same API key afterward.
The RP image must be the dedicated `.ota.uf2`; never use the generic SDK UF2.

Options:

| Option | Default | Meaning |
|---|---:|---|
| `--label=NAME` | `unnamed` | Identifies the firmware configuration |
| `--api-ms=N` | 1000 | Delay after each completed API request |
| `--page-ms=N` | 2000 | Delay after each completed page download |
| `--abort-ms=N` | 30000 | Delay after each interrupted request |
| `--metrics-ms=N` | 10000 | Maximum cadence of stored SSE samples |
| `--slow` | off | Pauses reads on the second SSE socket |
| `--overload` | off | Adds an aborted request; includes page downloads during OTA |
| `--baseline=REPORT` | none | Compare with a passing same-platform, same-workload report |
| `--ota=FIRMWARE_FILE` | none | Run the concurrent OTA phase and reboot |
| `--host-test` | off | Allow the host fixture on loopback only |

The delays are measured *after* each completed request; the effective rate falls
under load. Latency p95/p99 are estimated with a bounded reservoir of 8192 samples.
SSE parser storage, saved metric samples and recorded errors are bounded. Heap
minima are the minimum observed in SSE snapshots, which can miss shorter spikes;
the native allocator's lifetime minimum is also recorded at both boundaries.
The SSE cadence and maximum gap have explicit pass thresholds. Rejected API/page
requests, missing streams, gaps and request errors are reported. In overload
mode 429/503 rejections are counted separately and a recovery check must pass;
unexpected errors and a failed ordinary SSE stream still fail the test.

`--slow` stops the second socket's application reads. TCP/OS receive buffers
may still absorb data for a long time: a paused socket alone does not prove
that the device's transmit queue became full. Use long runs and correlate with
transport statistics or observed stalled writes before claiming backpressure was
exercised. The test does not close an existing user's stream to acquire slots.

`--baseline` rejects failed or different-platform/workload reports. A numerical
difference still is not a controlled experiment unless firmware, traffic,
hardware, Wi-Fi conditions and background clients are otherwise comparable.
Portal or other background probes are not disabled; report counters expose their
additional HTTP requests.

TX allocation and counter snapshots are captured when the firmware exposes
them. `tx_failures` combines failed jobs and rejected write attempts; the latter
can include retryable backpressure, so its increase alone does not prove data
loss. Separate failed/rejected counters are recorded when available. Observed
allocation maxima are sampled and may miss short peaks; the firmware's
`tx_peak_bytes` is a lifetime high-water mark, not a per-test peak.
An increase in separately reported `tx_failed_jobs` fails ordinary runs. In
overload or slow-reader runs it is reported but does not by itself fail the
run: expected cancellation or a stalled connection can increment this count.
The aggregate cannot distinguish expected teardown from another failure.

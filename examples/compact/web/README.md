# Compact device page

One static HTML page with inline CSS and JavaScript. No Vue, images, web fonts,
CDN, login barrier, browser credential storage, page routing or background HTTP
polling. Opening the page immediately loads `/ui/status` and opens SSE at
`/ui/events`; the UI sends no `X-Client-Key` header. Settings use JSON POST to
`/ui/config`; firmware uses raw File POST to `/ui/firmware`.

## Build

`node examples/compact/web/pack.mjs path/to/generated_web.hpp` embeds the local
SHA-256 helper, compresses the complete page and emits `compact_web::gzip` and
`compact_web::gzip_size`. Serve this array at `/` as `text/html; charset=utf-8`
with `Content-Encoding: gzip`. Generated headers belong in the build directory.
The source HTML uses a build insertion marker and is not the deployed artifact.
Current payload size is printed by the packer; this is separate from server
firmware size and runtime RAM.

## UI and external automation

The UI is open by the owner's explicit design. The API key protects external
`/api/*` clients only. The automation section starts with an empty readonly
field; pressing “Создать / заменить” explicitly POSTs `{}` as JSON to
`/ui/api-key`. A201 response provides `{key:"64 lowercase hex characters"}`.
The new key is shown only from this operation. Reload does not request creation
or rotation and leaves the field empty. The server retains the current key;
replacing it invalidates the old credential. The page stores no key in browser
storage or URLs. Copy uses Clipboard API when available and a selected-input
copy fallback on plain HTTP. If both fail, the value remains manually selectable.

Status has `name`, `firmware`, `uptime_ms`, `free_heap`, `min_free_heap`,
`rssi_dbm`, `led`, `interval_ms`, `firmware_format` and `max_firmware_bytes`.
SSE `data:` records carry the same JSON. A lost stream retries without starting
periodic HTTP polling. Configuration is at most256bytes: `name` (24 UTF-8 bytes),
`interval_ms` (1000–60000) and boolean `led`.

Firmware POST sends the original File as `application/octet-stream` with
`X-Firmware-SHA256` containing SHA-256 of the complete wire file. The incremental
browser helper also works on plain HTTP where SubtleCrypto is unavailable. It
reads File chunks to hash, then fetch sends the File directly; no base64/JSON
wrapper is used. Initial `firmware_format` selects `.bin` or `.uf2` and
`max_firmware_bytes` bounds the file. The UI displays the device's acknowledgement;
it does not independently prove a successful boot of the new image.

## Repeatable checks

```sh
node --test examples/compact/web/hash.test.mjs
node examples/compact/web/browser-test.mjs
```

The browser fixture verifies automatic startup without credentials, public UI
requests without a key header, configuration error/success geometry, SSE without
periodic HTTP, explicit key creation, plain-HTTP copy fallback, empty disclosure
and no key rotation after reload, plus a65536-byte upload with matching digest.
Light/dark/mobile screenshots and result remain ignored in
`local/compact-web-browser`. The fixture never flashes a physical device.

## Memory and CPU accounting

The status table displays allocator scope and aligned free/total KiB plus the
percentage free, calculated from the same SSE status record. Unknown totals or
inconsistent free/total values display dashes; they are never treated as zero
percent. The second allocator keeps its own permanent row. For RP, FreeRTOS heap
and newlib heap are separate pools, and neither is called all physical SRAM.
Minimum and largest contiguous block are shown in KiB. Static RAM and physical
SRAM occupy a separate row with a non-additive accounting explanation in its
tooltip: static RAM may already contain the RTOS heap's backing array.

Wire fields: `total_heap`, `free_heap`, `min_free_heap`, `largest_free_block`,
`heap_scope`, optional `secondary_heap_total`, `secondary_heap_free`,
`secondary_heap_scope`, `static_ram_bytes` and `physical_ram_bytes`. Zero unknown
static/physical totals display dashes. CPU uses `cpu_percent`, `cpu_available`
and `cpu_cores`; unavailable CPU is a dash, with active core count independent.
These fields add no requests: the initial status and the existing SSE stream
carry them. Browser acceptance covers unknown/zero totals, null CPU, large
numeric values, allocator labels and unchanged geometry across updates.

## Connection and bounded transmission

The wider view keeps device resources beside the connection and transmission
table; a narrow view retains the same aligned table rows. Each document counts
its own received SSE records and wire bytes, last-record age and reconnects.
An open connection without a record for more than the larger of five seconds
or three configured periods is marked stale. Neither this age timer nor a
reconnect starts HTTP polling. A manual button reads `/ui/metrics` once for the
server counters; absent optional values remain dashes.

`tx_bytes` and `tx_peak_bytes` describe allocated transmission payload memory,
not the entire server or SDK. `tx_memory_mode` is `lazy` (heap on demand), `heap`
(heap reserved at startup), or `static` (caller-owned static RAM).
`tx_reserved_bytes` is included in allocated bytes, never added to them;
`tx_limit_bytes` is a ceiling rather than an allocation. `tx_failures` combines
rejected and failed transmission jobs. `stream_clients`, `http_requests` and
`stream_records` are server-wide counters, independent of each document's
received-record counter. Missing fields do not claim zero usage.

The browser fixture also keeps two independent documents receiving SSE,
closes one while checking the other, exercises stale and reconnected states,
and verifies all three TX memory modes and manual-only metrics requests.
Error, success, missing values and reconnect checks preserve table/form geometry.

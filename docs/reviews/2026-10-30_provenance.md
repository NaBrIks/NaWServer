# Current snapshot provenance review — 2026-10-30

Scope: the current publishable tree, not a retroactive license change for old
snapshots. This is a technical source/provenance review, not a legal opinion or a
claim that a detector proves the absence of all possible copying.

## History and reference

The original extraction in the local historical repository included runtime
adaptations from ESPHome. Its 2026-10-19 review identified URL decoding, header/query
helpers, Basic authentication, the response hierarchy and an experimental SSE copy.
That review was read from the preserved history; deleted old implementations and
documents are not restored to the new publication.

The reference reviewed here is ESPHome tag **2025.3.0**, commit
`c4de9e87e416f04e6ae7a28eb503a65dae3bcc72`, specifically
[`web_server_idf`](https://github.com/esphome/esphome/tree/2025.3.0/esphome/components/web_server_idf)
and `web_server_base`. The tag is a comparison reference, not proof of the exact
historical import revision. Its [license](https://github.com/esphome/esphome/blob/2025.3.0/LICENSE)
places C++ runtime under GPLv3. Merely renaming or modifying such code would not
make it MIT; see the [GNU licensing FAQ](https://www.gnu.org/licenses/gpl-faq.html).

## Current code checked against the known findings

| Earlier area | Current implementation / review result |
|---|---|
| In-place, NUL-based URL decoder / hex helper | `src/http/url_decode.cpp`: explicit lengths, capacity/overlap/error contract, lookup or `from_chars`; old function/helper absent |
| Native header/query helper algorithms | `src/http/web_request.cpp`: backend-neutral buffer-status API, target views and transactional query/form parser; `src/backends/esp_idf_exchange.h` is a bounded SDK adapter |
| Basic/base64 authentication | `src/auth/basic_auth.cpp`: validates/decode-compares the RFC token group by group; no old decoded-string helper/cache |
| Async response hierarchy / stream formatting | `include/NaWServer/response.h`, `src/http/web_response.cpp`: one bounded response contract, explicit body lifetimes and fixed formatting buffer; obsolete hierarchy absent |
| Experimental copied SSE | Not in the current tree; `src/sse` has its own bounded encoder/session/claim lifecycle and adapters |
| HTTPD server / WS dispatch | `src/backends/esp_idf*.cpp`: lifecycle state, portable exchange/session interfaces and pre-101 reservation; API-required IDF calls are not treated as copying by themselves |
| Routing, middleware, body ownership | Compared with the historical extraction and upstream: shared parts originate in the owner's added pipeline/body/route mechanisms, not in the reference runtime |
| New TCP/memory/admission/JSON/MCP code | Separate portable wire/transport, move-owned blocks, fixed admission and caller-owned JSON/MCP contracts. Optional vendored parsers remain separately licensed |

All project `src/` and `include/` C/C++ files were scanned against the reference:
comments removed, token windows of 32 exact tokens and 80 tokens with non-keyword
identifiers normalized. **No shared windows at those thresholds were found.**
Comparison with the initial local extraction also found retained owner-authored
pipeline/body/facade code and ordinary IDF API sequences; each category was reviewed
separately. Thresholds are review aids, not a copyright test. Common protocol
constants, API calls, trivial checks and class names are not counted as evidence
of derivation without a substantive implementation match.

## License decision and boundaries

### S3 board code is separate from the server core

The hardware example is not inferred to be Apache simply because the Waveshare
repository root has that license. Its `waveshare_rgb_lcd_port.c` at commit
`b30c6be50d3d60b9426f84439cd368fe0499dbeb` (2026-07-25, before this demo was added)
explicitly carries `SPDX-License-Identifier: CC0-1.0` and
`SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD`.
The GPIO4 / CH422G 0x24/0x38, 0x2c/0x2e reset sequence and RGB parameters were
reviewed against that file. The current driver changes error propagation, uses
RTOS delays and separate backlight control, and adds measured buffer/timing policy.

| Current demo file / scope | Publication boundary |
|---|---|
| `examples/touch_lcd7/main/board.cpp`: reset/init integration | `MIT AND CC0-1.0`; owner's 2026 additions plus the explicitly attributed CC0 hardware-example basis |
| `examples/touch_lcd7/main/board.hpp`: project `Touch` structure / declarations | Owner-authored MIT API; not a copy of the manufacturer's large board header |
| `examples/touch_lcd7/main/graphics.cpp/.hpp`: test UI and frame synchronization | Owner-authored MIT integration calling external LVGL/IDF APIs; does not incorporate vendor UI or the vendor LVGL adapter |
| Board PDFs / manufacturer README in `docs/board` | Manufacturer repository materials retain the copied Apache-2.0 license; this says nothing about per-file CC0 code |

No automatic license decision is made for downstream MAIN/NaBTools/NaCBORUI
adaptations: their actual copied portions and later additions must be checked by
their owners. Previous GPL selection on the owner's own additions is distinct
from the upstream per-file license. The historical source and current headers
were examined; the new Git root is not used as license evidence.

The known copied implementations listed above are no longer distributed. This
release applies MIT to the current owner-authored implementations, under the
owner's explicit publication/relicensing instruction. It does not assert a
clean-room process: previous developers have seen the upstream code. No retained
GPL implementation has been identified in the reviewed current core. No waiver
of third-party rights is inferred from the new Git root.

All vendor copyright notices and licenses remain in place; see
[THIRD_PARTY_NOTICES](../../THIRD_PARTY_NOTICES.md). The scope includes yyjson,
RapidJSON/BSD notices, Tabler, frontend packages, SDK and manufacturer materials.
The upstream JSON-licensed RapidJSON test directory is not included. Former GPL
snapshots remain under their original terms in the verified local archive.

Limits: exact historical import revision is unknown; this is not an exhaustive
comparison with every upstream project or every historical revision. A future
concrete source match must be investigated on its own merits. Missing historical
copyright information cannot be repaired by deleting history.

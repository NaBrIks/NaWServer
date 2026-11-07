# Third-party code and materials

The root MIT license applies to current project-authored code. It does not replace
the licenses of the following code, assets, documentation or external dependencies.

| Item / scope | Origin | License / notice |
|---|---|---|
| Optional yyjson reader, `third_party/yyjson/src` | [ibireme/yyjson](https://github.com/ibireme/yyjson), pinned version in its [README](third_party/yyjson/README.md) | MIT, Copyright (c) 2020 YaoYuan; [license](third_party/yyjson/LICENSE), source notices retained |
| Optional RapidJSON headers, `third_party/rapidjson/include` | [Tencent/rapidjson](https://github.com/Tencent/rapidjson/tree/24b5e7a8b27f42fa16b96fc70aade9106cf7102f) | MIT plus the bundled BSD notices, including `msinttypes`; complete [upstream license](third_party/rapidjson/LICENSE) retained |
| RapidJSON raw-number adjustment | [Patch and explanation](third_party/rapidjson/README.md) | Adaptation of MIT RapidJSON; upstream notices remain. JSON-licensed `bin/jsonchecker` is not vendored |
| Tabler font/SVG icons, `frontend/src/design/icons` | [tabler/tabler-icons](https://github.com/tabler/tabler-icons) | MIT, Copyright (c) 2020–2024 Paweł Kuna; [notice](frontend/src/design/icons/LICENSE.txt), also served as `/licenses/Tabler-Icons.txt` |
| Vue, frontend only | [vuejs/core](https://github.com/vuejs/core) | MIT, Yuxi (Evan) You; [bundled notice](frontend/public/licenses/Vue.txt) |
| cborg, frontend only | [rvagg/cborg](https://github.com/rvagg/cborg) | Apache-2.0, Copyright 2020 Rod Vagg; [bundled notice and license](frontend/public/licenses/cborg.txt). Its JSON encoder also attributes MIT code to Feross Aboukhadijeh; that optional JSON entry is not imported by the demos |
| Frontend build/test tools | Vite, TypeScript, vue-tsc, Playwright and plugins, locked by npm | Build/development dependencies, not linked into the C++ core; retain each package's bundled license when redistributing those tools |
| ESP-IDF | [espressif/esp-idf](https://github.com/espressif/esp-idf) | External SDK: Apache-2.0 and component-specific licenses, including lwIP BSD; not relicensed by this repository |
| Pico SDK, RP2350 examples | [raspberrypi/pico-sdk 2.3.1](https://github.com/raspberrypi/pico-sdk/tree/2.3.1) | External SDK; [BSD-3-Clause SDK license](https://github.com/raspberrypi/pico-sdk/blob/2.3.1/LICENSE.TXT) and dependency-specific notices apply |
| CYW43 driver/firmware used by Pico SDK | SDK-pinned [georgerobotics/cyw43-driver](https://github.com/georgerobotics/cyw43-driver/tree/055d64274b014dd7b1c2fc94d26e8a18face7124) | Separate [LICENSE](https://github.com/georgerobotics/cyw43-driver/blob/055d64274b014dd7b1c2fc94d26e8a18face7124/LICENSE) and Raspberry Pi semiconductor-specific [LICENSE.RP](https://github.com/georgerobotics/cyw43-driver/blob/055d64274b014dd7b1c2fc94d26e8a18face7124/LICENSE.RP); these are not replaced by the NaWServer MIT license |
| FreeRTOS Kernel and RP2350 port | [FreeRTOS/FreeRTOS-Kernel V11.3.3](https://github.com/FreeRTOS/FreeRTOS-Kernel/tree/V11.3.3) and its pinned community-port submodule | External dependency; [MIT](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/V11.3.3/LICENSE.md) and individual port notices remain with upstream sources |
| LittleFS, RP storage adapters | [littlefs-project/littlefs v2.11.3](https://github.com/littlefs-project/littlefs/tree/v2.11.3) | External dependency, [BSD-3-Clause](https://github.com/littlefs-project/littlefs/blob/v2.11.3/LICENSE.md), Arm Limited / littlefs authors; no FS source is vendored here |
| Arm GNU toolchain / libstdc++ runtime | External compiler/runtime selected by the consuming application | GNU toolchain licenses and the GCC Runtime Library Exception apply to upstream components. The narrow project-authored gthread bridge does not relicense or vendor libstdc++ |
| LCD example dependencies | Per-example `main/idf_component.yml` files | External ESP-IDF components; their own manifests/licenses apply; not mandatory for the core |
| TinyCBOR, example firmware only | `espressif/cbor` `0.6.1~4`, declared by examples | MIT, Copyright (c) 2017 Intel Corporation; external component license retained by its package |
| LVGL, LCD example only | `lvgl/lvgl` 8.3.11 | MIT, Copyright (c) 2021 LVGL Kft; external component, not server-core code |
| LED strip / touch drivers, examples only | `espressif/led_strip` 3.0.1, `esp_lcd_touch` 1.2.1, `esp_lcd_touch_gt911` 1.1.3 | External Espressif components with Apache-2.0 licenses in their packages |
| Showcase .NET simulator/tests | .NET 10 SDK/runtime and `System.Formats.Cbor` 10.0.11 | External .NET/MIT package; not needed by the C++ library or controller firmware |
| Waveshare S3 board materials, `examples/touch_lcd7/docs/board` | [Manufacturer repository](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7) and linked manufacturer documentation | Retained manufacturer [Apache-2.0 license](examples/touch_lcd7/docs/board/LICENSE.txt); source links and PDF hashes in the local [index](examples/touch_lcd7/docs/board/README.md) |
| S3 board reset/init adaptation, `examples/touch_lcd7/main/board.cpp` | [Per-file CC0 example](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7/blob/b30c6be50d3d60b9426f84439cd368fe0499dbeb/examples/ESP-IDF/08_lvgl_v8_demo/components/waveshare_rgb_lcd_port.c) | Own code MIT; referenced reset/init portions CC0-1.0, SPDX-FileCopyrightText 2022 Espressif Systems (Shanghai) CO LTD. File marked `MIT AND CC0-1.0`; [CC0 text](LICENSES/CC0-1.0.txt). This is distinct from the manufacturer's root Apache license |
| Demo design tokens/tooltips | Owner's UI.TechnicalShell project; [design notes](frontend/src/design/README.md) | Project-authored integration; Tabler remains separately attributed as above |

Historical extracted runtime code was GPL-covered. The current publication does
not include those obsolete implementations or relicense old snapshots. See the
[dated provenance review](docs/reviews/2026-10-30_provenance.md). Do not remove a
notice merely because a name was changed or a Git history was restarted.

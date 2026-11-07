# Матрица сборок и измерений JSON

Уточнение 27.10: SDK `min_free_bytes` — сумма исторических минимумов отдельных
heap regions, достигнутых в разное время. Значение96 Б не доказывает одновременный
остаток96 Б всей internal RAM; минимум текущих free в ночных снимках S3 —6663 Б.

Сформировано: 2026-10-26T22:15:28.375Z. Источники — только сохранённые build/result JSON, sdkconfig, образы и JSONL под local/. Отсутствующие данные отмечены как «—»/«не измерено», значения не оцениваются.

Текущий checkout: HEAD `632dafdb3575b7f08e1639efaee956f583bfc59e`; рабочее дерево изменено. Это состояние не доказывает, из какого точного исходного состояния собран каждый старый образ. В профиле показывается записанная метка project_version, config SHA и BIN SHA отдельно; если метки исходного проекта нет, source-stage не установлен.

Запись о наличии BIN/ELF/size.json означает только наличие build-артефактов; runtime-приёмка из неё не выводится. Статические `.data + .bss` — сумма полей `size.layout[].parts[".data"].size` и `.bss.size`; отсутствующий size report даёт неизвестное значение, не оценку.

## Полные образы

| ID | Проект / board | SDK | Backend / reader | MCP | Workspace / input / output | HTTP stack | BIN | data+bss | project_version | config SHA-256 | BIN SHA-256 | status |
|---:|---|---|---|---:|---:|---:|---:|---:|---|---|---|---|
| 28 | NaWServer_showcase / esp32c3 | framework-espidf-v6.1 | TCP / bounded | да | 2,048 / 4,096 / 2,048 | 6,144 | 1,264,048 | 69,216 | 0.1.0-showcase | b63b22487d6b | 56a6085d2b9e | build artifacts present; no matching device snapshot |
| 29 | NaWServer_touch_lcd7 / esp32s3 | framework-espidf-v5.5.6 | TCP / bounded | да | 2,048 / 4,096 / 2,048 | 8,192 | 1,531,504 | 74,912 | 0.1.0-touch-lcd7 | fbedb1456c2a | 682c2ae56dcd | build artifacts present; matching point-in-time device snapshot at 2026-10-26T22:10:17.398Z |
| 30 | NaWServer_c6_lcd147 / esp32c6 | framework-espidf-v5.5.6 | TCP / bounded | да | 2,048 / 4,096 / 2,048 | 8,192 | 1,325,856 | 76,752 | 0.1.0-c6-demo | 11c65f72180c | 00cc72b6e770 | build artifacts present; matching point-in-time device snapshot at 2026-10-26T22:10:17.377Z |
| 31 | NaWServer_minimal / esp32c3 | framework-espidf-v5.5.6 | TCP / bounded | да | 2,048 / 4,096 / 2,048 | 6,144 | 991,568 | 52,644 | 0.2.0-dev | 483437576a82 | 055bee9b9090 | build artifacts present; no matching device snapshot |
| 32 | NaWServer_minimal / esp32c3 | framework-espidf-v6.1 | TCP / bounded | да | 2,048 / 4,096 / 2,048 | 6,144 | 1,005,680 | 54,512 | 0.2.0-dev | d0b4e776ee5b | 7a32736f4978 | build artifacts present; no matching device snapshot |
| 33 | NaWServer_c6_lcd147 / esp32c6 | framework-espidf-v5.5.6 | TCP / yyjson | да | 16,384 / 4,096 / 2,048 | 8,192 | 1,442,032 | 91,088 | 0.1.0-c6-demo | 1d9b1e7c1748 | a6816c12373a | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:33:02.417Z |
| 34 | NaWServer_touch_lcd7 / esp32s3 | framework-espidf-v5.5.6 | TCP / yyjson | да | 16,384 / 4,096 / 2,048 | 8,192 | 1,642,128 | 74,912 | 0.1.0-touch-lcd7 | 5037605268bc | 0f6af7ad0a43 | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:33:14.717Z |
| 35 | NaWServer_showcase / esp32c3 | framework-espidf-v6.1 | TCP / yyjson | да | 16,384 / 4,096 / 2,048 | 6,144 | 1,361,984 | 83,552 | 0.1.0-showcase | 69656bf5e81b | 1f3b38cc17c2 | build artifacts present; no matching device snapshot |
| 40 | NaWServer_c6_lcd147 / esp32c6 | framework-espidf-v5.5.6 | TCP / rapidjson | да | 8,192 / 4,096 / 2,048 | 8,192 | 1,336,672 | 82,896 | 0.1.0-c6-demo | 554ab31ee2c1 | 21f38f469ba2 | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:30:26.528Z |
| 41 | NaWServer_touch_lcd7 / esp32s3 | framework-espidf-v5.5.6 | TCP / rapidjson | да | 8,192 / 4,096 / 2,048 | 8,192 | 1,541,392 | 74,912 | 0.1.0-touch-lcd7 | 7a0a30747962 | ccef9a40573f | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:30:33.711Z |
| 42 | NaWServer_showcase / esp32c3 | framework-espidf-v6.1 | TCP / rapidjson | да | 8,192 / 4,096 / 2,048 | 8,192 | 1,274,912 | 75,360 | 0.1.0-showcase | 8eca2086d42f | 06cb190f16cb | build artifacts present; no matching device snapshot |
| 46 | NaWServer_c6_lcd147 / esp32c6 | framework-espidf-v5.5.6 | HTTPD / bounded | да | 2,048 / 4,096 / 2,048 | 8,192 | 1,348,336 | 76,720 | 0.1.0-c6-demo | 15a30092ec86 | 20c759445d65 | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:43:14.579Z |
| 47 | NaWServer_c6_lcd147 / esp32c6 | framework-espidf-v5.5.6 | HTTPD / yyjson | да | 16,384 / 4,096 / 2,048 | 8,192 | 1,464,528 | 91,056 | 0.1.0-c6-demo | cd2cd3e77f02 | 36cefedb1c09 | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:40:41.281Z |
| 48 | NaWServer_c6_lcd147 / esp32c6 | framework-espidf-v5.5.6 | HTTPD / rapidjson | да | 8,192 / 4,096 / 2,048 | 8,192 | 1,359,168 | 82,864 | 0.1.0-c6-demo | 26ff4e7d9e7a | 5806a11c89a2 | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:38:08.988Z |
| 49 | NaWServer_touch_lcd7 / esp32s3 | framework-espidf-v5.5.6 | HTTPD / bounded | да | 2,048 / 4,096 / 2,048 | 8,192 | 1,551,872 | 74,896 | 0.1.0-touch-lcd7 | 42571ac30b79 | 42caa394dd31 | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:49:15.029Z |
| 50 | NaWServer_touch_lcd7 / esp32s3 | framework-espidf-v5.5.6 | HTTPD / yyjson | да | 16,384 / 4,096 / 2,048 | 8,192 | 1,662,688 | 74,896 | 0.1.0-touch-lcd7 | 1f26b0a2fdab | d8dc007088e4 | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:41:33.241Z |
| 51 | NaWServer_touch_lcd7 / esp32s3 | framework-espidf-v5.5.6 | HTTPD / rapidjson | да | 8,192 / 4,096 / 2,048 | 8,192 | 1,561,728 | 74,896 | 0.1.0-touch-lcd7 | 9a5ea0f29c3e | c876fa9818de | build artifacts present; matching point-in-time device snapshot at 2026-10-26T13:38:48.995Z |

## Runtime snapshots, привязанные к ELF

В таблицу попадают только device snapshots, у которых info.details.build.elf_sha256 совпадает с ELF профиля, внешний ELF hash совпадает и boot ID в info/summary/tasks одинаков. Это точечные наблюдения при внешней нагрузке, не контролируемый memory delta и не доказательство отсутствия/наличия утечки.

| Профиль | Snapshot / tag | Reader / backend | Boot ID / IPv4 | RAM region | Total / free / min free / largest block | HTTP task stack min free |
|---:|---|---|---|---|---:|---:|
| 29 | 2026-10-26T22:10:17.398Z / night-final | nawserver_bounded / tcp | s3-def2b0f8-2336b160 / 192.0.2.2 | internal | 283,771 / 46,639 / 96 / 27,648 | 1,412 |
| 29 | 2026-10-26T22:10:17.398Z / night-final | nawserver_bounded / tcp | s3-def2b0f8-2336b160 / 192.0.2.2 | psram | 7,004,096 / 5,459,884 / 5,377,440 / 5,373,952 | 1,412 |
| 30 | 2026-10-26T22:10:17.377Z / night-final | nawserver_bounded / tcp | c6-c4e2c8cd-4c9033dd / 192.0.2.1 | internal | 335,196 / 184,928 / 108,960 / 151,552 | 1,936 |
| 33 | 2026-10-26T13:33:02.417Z / c6-profile33-20261026T133027-after-2 | yyjson / tcp | c6-947ea784-77771f01 / 192.0.2.1 | internal | 320,860 / 175,200 / 126,128 / 135,168 | 1,916 |
| 34 | 2026-10-26T13:33:14.717Z / s3-profile34-20261026T133034-after-2 | yyjson / tcp | s3-da5ff76f-e3a13c76 / 192.0.2.2 | internal | 283,771 / 41,611 / 296 / 20,480 | 900 |
| 34 | 2026-10-26T13:33:14.717Z / s3-profile34-20261026T133034-after-2 | yyjson / tcp | s3-da5ff76f-e3a13c76 / 192.0.2.2 | psram | 6,860,048 / 5,301,500 / 5,293,416 / 5,242,880 | 900 |
| 40 | 2026-10-26T13:30:26.528Z / c6-profile40-20261026T132753-after-2 | rapidjson / tcp | c6-955ac482-9f8192e5 / 192.0.2.1 | internal | 329,052 / 183,212 / 134,448 / 147,456 | 1,948 |
| 41 | 2026-10-26T13:30:33.711Z / s3-profile41-20261026T132753-after-2 | rapidjson / tcp | s3-ea90c8b9-7ebe711d / 192.0.2.2 | internal | 283,771 / 39,811 / 244 / 14,848 | 1,396 |
| 41 | 2026-10-26T13:30:33.711Z / s3-profile41-20261026T132753-after-2 | rapidjson / tcp | s3-ea90c8b9-7ebe711d / 192.0.2.2 | psram | 6,933,248 / 5,382,892 / 5,381,160 / 5,242,880 | 1,396 |
| 46 | 2026-10-26T13:43:14.579Z / c6-profile46-20261026T134042-after-2 | nawserver_bounded / idf_httpd | c6-ea834b60-c0264533 / 192.0.2.1 | internal | 335,004 / 222,148 / 174,420 / 180,224 | — |
| 47 | 2026-10-26T13:40:41.281Z / c6-profile47-20261026T133810-after-2 | yyjson / idf_httpd | c6-0df911c5-128d3b45 / 192.0.2.1 | internal | 320,668 / 208,816 / 154,856 / 159,744 | — |
| 48 | 2026-10-26T13:38:08.988Z / c6-profile48-20261026T133537-after-2 | rapidjson / idf_httpd | c6-e3358d14-4f9937f3 / 192.0.2.1 | internal | 328,860 / 215,836 / 152,080 / 180,224 | — |
| 49 | 2026-10-26T13:49:15.029Z / s3-profile49-20261026T134134-after-4 | nawserver_bounded / idf_httpd | s3-90354df1-62dd44ab / 192.0.2.2 | internal | 283,787 / 67,323 / 7,976 / 31,744 | — |
| 49 | 2026-10-26T13:49:15.029Z / s3-profile49-20261026T134134-after-4 | nawserver_bounded / idf_httpd | s3-90354df1-62dd44ab / 192.0.2.2 | psram | 6,928,408 / 5,384,196 / 5,384,024 / 5,242,880 | — |
| 50 | 2026-10-26T13:41:33.241Z / s3-profile50-20261026T133850-after-2 | yyjson / idf_httpd | s3-ccd2f347-8e0c576c / 192.0.2.2 | internal | 283,787 / 68,047 / 8,824 / 31,744 | — |
| 50 | 2026-10-26T13:41:33.241Z / s3-profile50-20261026T133850-after-2 | yyjson / idf_httpd | s3-ccd2f347-8e0c576c / 192.0.2.2 | psram | 6,849,896 / 5,291,348 / 5,291,176 / 5,242,880 | — |
| 51 | 2026-10-26T13:38:48.995Z / s3-profile51-20261026T133606-after-2 | rapidjson / idf_httpd | s3-6e6bd680-f9db3b74 / 192.0.2.2 | internal | 283,787 / 69,659 / 8,892 / 31,744 | — |
| 51 | 2026-10-26T13:38:48.995Z / s3-profile51-20261026T133606-after-2 | rapidjson / idf_httpd | s3-6e6bd680-f9db3b74 / 192.0.2.2 | psram | 6,923,096 / 5,372,740 / 5,372,568 / 5,242,880 | — |

Сравнения свободной PSRAM между разными образами не трактуются как эффект JSON-backend: размещение rodata меняет общий пул, в том числе примерно на 64 KiB. Ошибочные/непривязанные snapshot перечислены в JSON matrix.rejectedSnapshots.

## Изолированный JSON benchmark

Ниже приведены только сохранённые результаты профилей 36–39 и 43–45. `ok` — поле самого benchmark JSON, а не общая приёмка прошивки.

| ID | Board / SDK / reader | Arena reserved / observed peak | elapsed us | checksum | app consumer | result | config SHA-256 |
|---:|---|---:|---:|---:|---|---|---|
| 36 | esp32c6 / framework-espidf-v5.5.6 / nawserver_bounded | 2,048 / 1,016 | 334,462 | 257,600 | none | recorded benchmark result: ok=true | 06f5222b2646 |
| 37 | esp32c6 / framework-espidf-v5.5.6 / yyjson | 16,384 / 4,216 | 243,340 | 257,600 | none | recorded benchmark result: ok=true | 042112f0d0b0 |
| 38 | esp32c6 / framework-espidf-v5.5.6 / nawserver_bounded | 2,048 / 1,016 | 426,415 | 723,800 | yyjson | recorded benchmark result: ok=true | 06f5222b2646 |
| 39 | esp32c6 / framework-espidf-v5.5.6 / yyjson | 16,384 / 4,216 | 325,047 | 723,800 | yyjson | recorded benchmark result: ok=true | 042112f0d0b0 |
| 43 | esp32c6 / framework-espidf-v5.5.6 / rapidjson | 8,192 / 2,126 | 347,419 | 257,600 | none | recorded benchmark result: ok=true | 68bea1907048 |
| 44 | esp32c6 / framework-espidf-v5.5.6 / nawserver_bounded | 2,048 / 1,016 | 877,141 | 4,239,406,539,350,723,136 | rapidjson | recorded benchmark result: ok=true | 845e881c7f66 |
| 45 | esp32c6 / framework-espidf-v5.5.6 / rapidjson | 8,192 / 2,126 | 899,038 | 4,239,406,539,350,723,136 | rapidjson | recorded benchmark result: ok=true | 68bea1907048 |

## Сохранённые JSONL load records

Это механическое извлечение счётчиков из журналов. Smoke/harness-прогоны показаны отдельно и не считаются результатом финальной прошивки; counters.errors отражает данные harness и сам по себе не локализует причину.
Ограничение версии harness: ws_echo_sent и ws_echo_received увеличиваются вместе только после подтверждённого точного echo. Их равенство не доказывает доставку всех попыток; используйте число завершённых WS-обменов вместе с errors/timeouts. body_integrity_failures относится к HTTP-проверкам, а ошибки WS decoder/echo учитываются в общем errors. Ограниченная корзина error_reason_counts может объединять разные причины в other_capped.

| Файл / run | Класс | Клиенты / длительность | HTTP | WS open / sent / received | SSE open / events | Errors / timeouts | Boot ID | Recovery | Активы из ответов |
|---|---|---:|---:|---:|---:|---:|---|---|---|
| c6-profile30-20261026T133303-2.jsonl / 20261026T133334Z-130196 | final-load-record | 2 / 120,902 ms | 296 | 19 / 54 / 54 | 1 / 119 | 27 / 21 | c6-fcc7f986-d23ce7ac | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-profile33-20261026T133027-2.jsonl / 20261026T133100Z-128340 | final-load-record | 2 / 121,231 ms | 290 | 18 / 61 / 61 | 1 / 118 | 22 / 12 | c6-947ea784-77771f01 | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-profile40-20261026T132753-2.jsonl / 20261026T132825Z-141380 | final-load-record | 2 / 120,446 ms | 299 | 16 / 57 / 57 | 1 / 120 | 17 / 6 | c6-955ac482-9f8192e5 | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-profile46-20261026T134042-2.jsonl / 20261026T134113Z-67508 | final-load-record | 2 / 120,161 ms | 812 | 1 / 374 / 374 | 1 / 119 | 4 / 4 | c6-ea834b60-c0264533 | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-profile47-20261026T133810-2.jsonl / 20261026T133839Z-138896 | final-load-record | 2 / 121,054 ms | 327 | 1 / 163 / 163 | 1 / 117 | 8 / 8 | c6-0df911c5-128d3b45 | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-profile48-20261026T133537-2.jsonl / 20261026T133608Z-143748 | final-load-record | 2 / 120,277 ms | 305 | 1 / 154 / 154 | 1 / 118 | 7 / 7 | c6-e3358d14-4f9937f3 | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-rapid-final-smoke-2.jsonl / 20261026T131937Z-30300 | harness-smoke-with-errors | 2 / 20,408 ms | 140 | 2 / 41 / 41 | 1 / 19 | 1 / 0 | c6-8450b284-893a3fdb | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-rapid-gate-smoke-4.jsonl / 20261026T132304Z-101544 | harness-smoke-with-errors | 4 / 20,192 ms | 119 | 4 / 40 / 40 | 2 / 40 | 2 / 2 | c6-8450b284-893a3fdb | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-rapid-smoke3-2.jsonl / 20261026T131053Z-78196 | harness-smoke-with-errors | 2 / 20,395 ms | 160 | 21 / 28 / 28 | 1 / 20 | 20 / 0 | c6-8450b284-893a3fdb | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-smoke-2.jsonl / 20261026T130104Z-92800 | harness-smoke-with-errors | 2 / 20,189 ms | 169 | 0 / 0 / 0 | 1 / 20 | 19 / 0 | c6-0fc0dc4b-1f592077 | health=200, info=200 | configured: —; observed: — |
| c6-smoke-fixed-2.jsonl / 20261026T130423Z-150444 | harness-smoke-with-errors | 2 / 20,166 ms | 177 | 21 / 59 / 59 | 1 / 20 | 20 / 0 | c6-0fc0dc4b-1f592077 | health=200, info=200 | configured: /; observed: / |
| c6-soak-cached-20261026T174455-gate-2.jsonl / 20261026T174528Z-144536 | final-load-record | 2 / 60,115 ms | 496 | 1 / 227 / 227 | 1 / 56 | 2 / 2 | c6-c4e2c8cd-4c9033dd | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-soak-cached-observe-20261026T180932-4.jsonl / 20261026T180933Z-172036 | final-load-record | 4 / 14,400,203 ms | 235,407 | 2,094 / 78,888 / 78,888 | 2 / 28,798 | 2,139 / 2,134 | c6-c4e2c8cd-4c9033dd | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-soak-default-20261026T134315-4.jsonl / 20261026T134447Z-69920 | final-load-record | 4 / 14,400,297 ms | 135,205 | 2,006 / 46,776 / 46,776 | 2 / 28,798 | 2,045 / 2,029 | c6-4e9f11e8-b76244c7 | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| c6-soak-default-20261026T134315-gate-2.jsonl / 20261026T134346Z-115064 | final-load-record | 2 / 60,343 ms | 501 | 1 / 168 / 168 | 1 / 59 | 0 / 0 | c6-4e9f11e8-b76244c7 | health=200, info=200 | configured: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-BahTyJsE.js, /assets/index-Bj0x0ohh.css |
| s3-profile29-20261026T133315-2.jsonl / 20261026T133404Z-126652 | final-load-record | 2 / 120,138 ms | 440 | 12 / 152 / 152 | 1 / 116 | 21 / 16 | s3-bc8fec67-d3789d47 | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-profile34-20261026T133034-2.jsonl / 20261026T133113Z-70036 | final-load-record | 2 / 120,112 ms | 384 | 17 / 127 / 127 | 1 / 117 | 29 / 17 | s3-da5ff76f-e3a13c76 | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-profile41-20261026T132753-2.jsonl / 20261026T132832Z-51540 | final-load-record | 2 / 120,111 ms | 424 | 7 / 143 / 143 | 1 / 116 | 8 / 4 | s3-ea90c8b9-7ebe711d | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-profile49-20261026T134134-2.jsonl / 20261026T134212Z-110832 | final-load-record | 2 / 120,381 ms | 1,059 | 1 / 476 / 476 | 1 / 120 | 0 / 0 | s3-90354df1-62dd44ab | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-profile49-20261026T134134-4.jsonl / 20261026T134414Z-177060 | final-load-record | 4 / 300,164 ms | 4,978 | 2 / 2,317 / 2,317 | 2 / 591 | 0 / 0 | s3-90354df1-62dd44ab | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-profile50-20261026T133850-2.jsonl / 20261026T133931Z-70604 | final-load-record | 2 / 120,353 ms | 561 | 2 / 278 / 278 | 1 / 119 | 5 / 5 | s3-ccd2f347-8e0c576c | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-profile51-20261026T133606-2.jsonl / 20261026T133647Z-109116 | final-load-record | 2 / 121,045 ms | 561 | 5 / 256 / 256 | 1 / 119 | 9 / 9 | s3-6e6bd680-f9db3b74 | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-rapid-final-smoke-2.jsonl / 20261026T131937Z-131880 | harness-smoke-with-errors | 2 / 20,241 ms | 158 | 2 / 47 / 47 | 1 / 19 | 1 / 0 | s3-b55c26b3-39e00170 | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-rapid-gate-smoke-4.jsonl / 20261026T132304Z-47448 | harness-smoke-with-errors | 4 / 20,342 ms | 53 | 8 / 17 / 17 | 2 / 38 | 17 / 17 | s3-b55c26b3-39e00170 | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-soak-cached-20261026T135101-gate-2.jsonl / 20261026T135138Z-123808 | final-load-record | 2 / 60,097 ms | 529 | 1 / 237 / 237 | 1 / 58 | 1 / 1 | s3-def2b0f8-2336b160 | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-soak-cached-observe-20261026T141019-4.jsonl / 20261026T141019Z-176384 | final-load-record | 4 / 14,400,152 ms | 247,068 | 439 / 108,910 / 108,910 | 2 / 28,241 | 1,354 / 1,346 | s3-def2b0f8-2336b160 | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |
| s3-soak-default-20261026T134915-gate-2.jsonl / 20261026T134952Z-61100 | final-load-record | 2 / 60,187 ms | 524 | 1 / 230 / 230 | 1 / 59 | 1 / 1 | s3-421e70ae-3245f2f8 | health=200, info=200 | configured: /assets/index-KXWHmjX3.js, /assets/index-Bj0x0ohh.css; observed: /assets/index-Bj0x0ohh.css, /assets/index-KXWHmjX3.js |

## Memory sampler JSONL

Агрегации строятся только из записей type=memory_sample; source/profile сопоставляется по ELF SHA-256, если sampler его записал. Свободная память показана механически по boot ID и region. Наличие короткого sampler JSONL само по себе не подтверждает ночной soak.

| Файл | Profile match | Samples / boot changes / errors | Boot IDs | Region | First / min / max / last free |
|---|---|---:|---|---|---:|
| c6-profile30-20261026T133303-2-memory.jsonl | 30 (matched by ELF SHA-256) | 2 / 0 / 0 | c6-fcc7f986-d23ce7ac | internal @ c6-fcc7f986-d23ce7ac (2 samples) | 195,244 / 168,848 / 195,244 / 168,848 |
| c6-profile33-20261026T133027-2-memory.jsonl | 33 (matched by ELF SHA-256) | 2 / 0 / 0 | c6-947ea784-77771f01 | internal @ c6-947ea784-77771f01 (2 samples) | 182,100 / 156,288 / 182,100 / 156,288 |
| c6-profile40-20261026T132753-2-memory.jsonl | 40 (matched by ELF SHA-256) | 2 / 0 / 0 | c6-955ac482-9f8192e5 | internal @ c6-955ac482-9f8192e5 (2 samples) | 174,076 / 159,736 / 174,076 / 159,736 |
| c6-profile46-20261026T134042-2-memory.jsonl | 46 (matched by ELF SHA-256) | 2 / 0 / 1 | c6-ea834b60-c0264533 | internal @ c6-ea834b60-c0264533 (1 samples) | 208,940 / 208,940 / 208,940 / 208,940 |
| c6-profile47-20261026T133810-2-memory.jsonl | 47 (matched by ELF SHA-256) | 2 / 0 / 1 | c6-0df911c5-128d3b45 | internal @ c6-0df911c5-128d3b45 (1 samples) | 209,320 / 209,320 / 209,320 / 209,320 |
| c6-profile48-20261026T133537-2-memory.jsonl | 48 (matched by ELF SHA-256) | 2 / 0 / 1 | c6-e3358d14-4f9937f3 | internal @ c6-e3358d14-4f9937f3 (1 samples) | 215,556 / 215,556 / 215,556 / 215,556 |
| c6-sampler-smoke-memory.jsonl | — (unmatched: no matching ELF SHA-256) | 1 / 0 / 0 | c6-8450b284-893a3fdb | summary.memory[0] @ c6-8450b284-893a3fdb (1 samples) | 166,468 / 166,468 / 166,468 / 166,468 |
| c6-soak-cached-20261026T174455-gate-2-memory.jsonl | 30 (matched by ELF SHA-256) | 1 / 0 / 0 | c6-c4e2c8cd-4c9033dd | internal @ c6-c4e2c8cd-4c9033dd (1 samples) | 189,772 / 189,772 / 189,772 / 189,772 |
| c6-soak-cached-observe-20261026T180932-4-memory.jsonl | 30 (matched by ELF SHA-256) | 237 / 0 / 0 | c6-c4e2c8cd-4c9033dd | internal @ c6-c4e2c8cd-4c9033dd (237 samples) | 183,248 / 152,568 / 183,248 / 166,632 |
| c6-soak-default-20261026T134315-4-memory.jsonl | 30 (matched by ELF SHA-256) | 236 / 0 / 0 | c6-4e9f11e8-b76244c7 | internal @ c6-4e9f11e8-b76244c7 (236 samples) | 166,352 / 153,836 / 175,460 / 170,448 |
| c6-soak-default-20261026T134315-gate-2-memory.jsonl | 30 (matched by ELF SHA-256) | 1 / 0 / 0 | c6-4e9f11e8-b76244c7 | internal @ c6-4e9f11e8-b76244c7 (1 samples) | 196,596 / 196,596 / 196,596 / 196,596 |
| s3-profile29-20261026T133315-2-memory.jsonl | 29 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-bc8fec67-d3789d47 | internal @ s3-bc8fec67-d3789d47 (2 samples) | 22,187 / 22,187 / 32,915 / 32,915 |
| s3-profile29-20261026T133315-2-memory.jsonl | 29 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-bc8fec67-d3789d47 | psram @ s3-bc8fec67-d3789d47 (2 samples) | 5,459,884 / 5,459,884 / 5,459,884 / 5,459,884 |
| s3-profile34-20261026T133034-2-memory.jsonl | 34 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-da5ff76f-e3a13c76 | internal @ s3-da5ff76f-e3a13c76 (2 samples) | 37,619 / 29,155 / 37,619 / 29,155 |
| s3-profile34-20261026T133034-2-memory.jsonl | 34 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-da5ff76f-e3a13c76 | psram @ s3-da5ff76f-e3a13c76 (2 samples) | 5,301,500 / 5,301,500 / 5,301,500 / 5,301,500 |
| s3-profile41-20261026T132753-2-memory.jsonl | 41 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-ea90c8b9-7ebe711d | internal @ s3-ea90c8b9-7ebe711d (2 samples) | 41,119 / 24,843 / 41,119 / 24,843 |
| s3-profile41-20261026T132753-2-memory.jsonl | 41 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-ea90c8b9-7ebe711d | psram @ s3-ea90c8b9-7ebe711d (2 samples) | 5,382,892 / 5,382,892 / 5,382,892 / 5,382,892 |
| s3-profile49-20261026T134134-2-memory.jsonl | 49 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-90354df1-62dd44ab | internal @ s3-90354df1-62dd44ab (2 samples) | 64,579 / 53,891 / 64,579 / 53,891 |
| s3-profile49-20261026T134134-2-memory.jsonl | 49 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-90354df1-62dd44ab | psram @ s3-90354df1-62dd44ab (2 samples) | 5,384,196 / 5,384,196 / 5,384,196 / 5,384,196 |
| s3-profile49-20261026T134134-4-memory.jsonl | 49 (matched by ELF SHA-256) | 5 / 0 / 0 | s3-90354df1-62dd44ab | internal @ s3-90354df1-62dd44ab (5 samples) | 47,003 / 42,867 / 50,115 / 44,743 |
| s3-profile49-20261026T134134-4-memory.jsonl | 49 (matched by ELF SHA-256) | 5 / 0 / 0 | s3-90354df1-62dd44ab | psram @ s3-90354df1-62dd44ab (5 samples) | 5,384,196 / 5,384,196 / 5,384,196 / 5,384,196 |
| s3-profile50-20261026T133850-2-memory.jsonl | 50 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-ccd2f347-8e0c576c | internal @ s3-ccd2f347-8e0c576c (2 samples) | 66,659 / 45,883 / 66,659 / 45,883 |
| s3-profile50-20261026T133850-2-memory.jsonl | 50 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-ccd2f347-8e0c576c | psram @ s3-ccd2f347-8e0c576c (2 samples) | 5,291,348 / 5,291,348 / 5,291,348 / 5,291,348 |
| s3-profile51-20261026T133606-2-memory.jsonl | 51 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-6e6bd680-f9db3b74 | internal @ s3-6e6bd680-f9db3b74 (2 samples) | 57,347 / 40,711 / 57,347 / 40,711 |
| s3-profile51-20261026T133606-2-memory.jsonl | 51 (matched by ELF SHA-256) | 2 / 0 / 0 | s3-6e6bd680-f9db3b74 | psram @ s3-6e6bd680-f9db3b74 (2 samples) | 5,372,740 / 5,372,740 / 5,372,740 / 5,372,740 |
| s3-soak-cached-20261026T135101-gate-2-memory.jsonl | 29 (matched by ELF SHA-256) | 1 / 0 / 0 | s3-def2b0f8-2336b160 | internal @ s3-def2b0f8-2336b160 (1 samples) | 47,727 / 47,727 / 47,727 / 47,727 |
| s3-soak-cached-20261026T135101-gate-2-memory.jsonl | 29 (matched by ELF SHA-256) | 1 / 0 / 0 | s3-def2b0f8-2336b160 | psram @ s3-def2b0f8-2336b160 (1 samples) | 5,459,884 / 5,459,884 / 5,459,884 / 5,459,884 |
| s3-soak-cached-observe-20261026T141019-4-memory.jsonl | 29 (matched by ELF SHA-256) | 238 / 0 / 0 | s3-def2b0f8-2336b160 | internal @ s3-def2b0f8-2336b160 (238 samples) | 45,451 / 6,663 / 45,451 / 22,947 |
| s3-soak-cached-observe-20261026T141019-4-memory.jsonl | 29 (matched by ELF SHA-256) | 238 / 0 / 0 | s3-def2b0f8-2336b160 | psram @ s3-def2b0f8-2336b160 (238 samples) | 5,459,884 / 5,389,892 / 5,459,884 / 5,459,884 |
| s3-soak-default-20261026T134915-gate-2-memory.jsonl | 29 (matched by ELF SHA-256) | 1 / 0 / 0 | s3-421e70ae-3245f2f8 | internal @ s3-421e70ae-3245f2f8 (1 samples) | 43,755 / 43,755 / 43,755 / 43,755 |
| s3-soak-default-20261026T134915-gate-2-memory.jsonl | 29 (matched by ELF SHA-256) | 1 / 0 / 0 | s3-421e70ae-3245f2f8 | psram @ s3-421e70ae-3245f2f8 (1 samples) | 5,459,884 / 5,459,884 / 5,459,884 / 5,459,884 |

Финальных (не-smoke) записей с recovery: 20. Текущая рабочая копия может быть dirty; SHA конфигурации/образа фиксирует именно сохранённый файл, а не точную историю исходников.

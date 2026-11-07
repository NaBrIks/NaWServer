# Журнал сборок и памяти ESP

Записи добавляются по мере проверки. Конфигурация и цифры относятся только к
указанному профилю, SDK и состоянию исходников. Это журнал измерений, а не
заявление о поддержке всех версий IDF или аппаратной стабильности.

Flash и статическая память берутся из фактического linker map через установленный
`esp_idf_size 2.2.1`; размер BIN указан отдельно. Таблица сохраняет названия областей
самого инструмента: код в IRAM/DIRAM тоже занимает оперативную память.
`data+bss` — только статические данные, без стека, heap и размещённого в RAM кода.
Runtime heap, minimum heap и largest block записываются отдельно после проверки
реального устройства; они не выводятся из ELF и не подменяются статическими размерами.

## 2026-11-02 — общий HTTP/WS/SSE TX, C3/S3 и RP

Ветка `feature/unified-tx-scheduler`, срез после `4e80b37` и исправлений
fairness/диагностики. Новые SDK и toolchain в этой проверке не устанавливались.
Полная методика, результаты и открытые отказы:
[аппаратный отчёт](readme_20261102_unified_tx_verification_ru.md).

В этом срезе модуль `esp_idf_size` недоступен в используемом окружении.
Для ESP ниже указаны только фактические `.dram0.data` и `.dram0.bss` из linker map,
без попытки назвать их всей статической/общей RAM.

| Профиль | SDK / компилятор | BIN, Б | DRAM data, Б | DRAM bss, Б | Сумма, Б |
| --- | --- | ---: | ---: | ---: | ---: |
| C3 Showcase TCP | IDF 5.5.6 / RISC-V GCC 14.2.0+20260121 | 1153696 | 17348 | 34944 | 52292 |
| S3 Minimal TCP | IDF 5.5.6 / Xtensa GCC 14.2.0+20251107 | 916976 | 19460 | 23168 | 42628 |

Оба ESP-профиля: `CONFIG_COMPILER_OPTIMIZATION_PERF=y`, фактически `-O2`;
assertions оставлены. SDKCONFIG SHA256-prefix: C3 `ee72463f3448`, S3 `8c8988cc0305`.
WS/SSE/TX включены, admission в этих аппаратных профилях выключен.
C3 tick 1000 Гц; S3 tick 100 Гц. C3: Wi-Fi dynamic TX 32, TCP send buffer
5760 Б, TCP_OVERSIZE_MSS; экспериментальные 8/2880/oversize-off не оставлены.

C3 TX: 32768 Б / 12 jobs, block 5760 Б, step 1440 Б, максимум 2 jobs на
соединение, резерв 1 важного job и 5760 Б. Payload выделяется лениво и возвращается.
Showcase Flash assets передаются `sendImmutable`; заимствованные байты и память
payload считаются отдельно. Проверяемый timeout-профиль C3 — 3000 мс для
no-progress, WS frame, TX stall/close; общие defaults библиотеки не увеличены.

C3 в умеренном 30-секундном сценарии: CPU 20,02%, TX failures +0; в egress stress
61,88%, TX failures +0. Оба с внешней старой вкладкой. Предельный duplex давал
обрывы; после неудачного повтора free internal 65328 Б, minimum 3644 Б, largest
23552 Б. Это накопительный минимум одного boot с несколькими сценариями, не
«потребление одной очереди». Наблюдавшийся TX payload peak около 7497 Б.
Минимумы свободного stack C3: main 476, HTTP 1096, TX 3484, WS 2480, SSE 1796 Б.
Стек TX всего 4096 Б — кандидат на отдельное уменьшение после проверки редких путей.

S3 Minimal использует main stack 12288 Б и HTTP stack 8192 Б после обнаруженного
ранее overflow при SSE; общие stacks библиотеки не увеличивались. Финальный LAN
прогон прошёл; полный финальный heap-профиль Minimal этим прогоном не собирался.
Записывалось только приложение `0x10000..0xefdef` включительно. Таблица разделов,
NVS и общий `project` SPIFFS сохранены.

Pico 2 W: Pico SDK 2.3.1 / Arm GCC 14.2.Rel1 / FreeRTOS 11.3.3,
Release `-O3`, BIN 783876 Б. GNU `size` для ELF: text 787972, data 0, bss 311732 Б;
RP bss включает статический FreeRTOS heap и не сопоставляется с ESP data+bss
как «накладные расходы библиотеки». Код помещается в первые 2 МиБ, LittleFS
1835008 Б и NVS 262144 Б остаются отдельными областями.
TX 32768 Б / 12 jobs, block 5840 Б, step 1460 Б; HTTP connection limit 10,
WS limit 4. В умеренном сценарии CPU обоих ядер, нормированный к 100%, — 16,90%;
FreeRTOS heap free 145800 Б, minimum 144672 Б, largest 145712 Б. После предельного
stress TX peak 19102 Б, были ошибки передачи — высокая свободная RAM не является
доказательством приёмки.

Исходные ESP-сборки текущей задачи были `-Og`; их CPU нельзя использовать как
контроль для приписывания выигрыша только новому TX. Итоговый статус — функциональная
проверка трёх плат выполнена, умеренные сценарии прошли, предельная смешанная
нагрузка и чистая idle/редкий API матрица остаются открытыми.

## 2026-10-26 — переход на общие местные SDK

- Ветка: `codex/http-backend-adapters`, исходная точка `632dafd`.
- SDK: общие `framework-espidf-v5.5.6` и `framework-espidf-v6.1` в
  `%USERPROFILE%\.platformio\packages`; новых установок в этой проверке нет.
- RISC-V GCC: `14.2.0+20260121` для IDF 5.5.6, `15.2.0+20251204` для IDF 6.1.
- Xtensa GCC: имеющийся `14.2.0+20251107`.
- Полные проверки выполняются CMake/Ninja с раздельными SDKCONFIG/build-каталогами.
  Уже имеющиеся managed components подключаются явно; по умолчанию Component Manager
  отключён. Для S3 Touch включён штатный установленный менеджер, поскольку GT911
  объявляет транзитивную зависимость в YAML. Проверка новых версий отключена;
  используются прежние версии компонентов. Исходники приложения не подменяются.
- Отдельная 16-профильная проверка WS/SSE — компиляция исходников компонента
  и минимального потребителя с настоящими SDK headers/flags; её нельзя выдавать
  за полную линковку firmware или аппаратную проверку.

<!-- backend26-0-AACFFDD96075 -->
### 2026-10-26 09:41:11 +05:00 — NaWServer_minimal, esp32c3, SDK 5.5.6, профиль 0

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/0`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **964960 байт**; linker total_size: **964606 байт**; статические data+bss: **48340 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 722012 | 0 | 0 |
| Flash Data | 146944 | 0 | 0 |
| DRAM | 131650 | 321296 | 189646 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-4-EF1D507A869F -->
### 2026-10-26 09:41:12 +05:00 — NaWServer_showcase, esp32c3, SDK 5.5.6, профиль 4

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/4`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1114928 байт**; linker total_size: **1114564 байт**; статические data+bss: **63540 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 752516 | 0 | 0 |
| Flash Data | 246344 | 0 | 0 |
| DRAM | 161900 | 321296 | 159396 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-8-A782433F6A9A -->
### 2026-10-26 09:41:14 +05:00 — NaWServer_compile_check, esp32c3, SDK 5.5.6, профиль 8

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/8`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **389168 байт**; linker total_size: **382900 байт**; статические data+bss: **11992 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 258022 | 0 | 0 |
| Flash Data | 68348 | 0 | 0 |
| DRAM | 62598 | 321296 | 258698 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-13-AACFFDD96075 -->
### 2026-10-26 09:45:56 +05:00 — NaWServer_minimal, esp32c3, SDK 5.5.6, профиль 13

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/13`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **966080 байт**; linker total_size: **965722 байт**; статические data+bss: **48340 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 723128 | 0 | 0 |
| Flash Data | 146944 | 0 | 0 |
| DRAM | 131650 | 321296 | 189646 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- feature26-5.5.6-HTTPD-http -->
### 2026-10-26 — compile-only 5.5.6-HTTPD-http

```ini
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
# CONFIG_NAWSERVER_SSE is not set
```
Результат: PASS, скомпилировано объектов NaWServer/потребителя: 16.
Полной линковки образа в этой строке нет; Flash/RAM по ней не измеряются.

<!-- feature26-5.5.6-HTTPD-ws -->
### 2026-10-26 — compile-only 5.5.6-HTTPD-ws

```ini
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
# CONFIG_NAWSERVER_SSE is not set
```
Результат: PASS, скомпилировано объектов NaWServer/потребителя: 17.
Полной линковки образа в этой строке нет; Flash/RAM по ней не измеряются.

<!-- feature26-5.5.6-HTTPD-sse -->
### 2026-10-26 — compile-only 5.5.6-HTTPD-sse

```ini
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_SSE=y
```
Результат: PASS, скомпилировано объектов NaWServer/потребителя: 21.
Полной линковки образа в этой строке нет; Flash/RAM по ней не измеряются.

<!-- feature26-5.5.6-HTTPD-ws_sse -->
### 2026-10-26 — compile-only 5.5.6-HTTPD-ws_sse

```ini
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```
Результат: PASS, скомпилировано объектов NaWServer/потребителя: 22.
Полной линковки образа в этой строке нет; Flash/RAM по ней не измеряются.

<!-- feature26-5.5.6-TCP-http -->
### 2026-10-26 — compile-only 5.5.6-TCP-http

```ini
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
```
Результат: PASS, скомпилировано объектов NaWServer/потребителя: 20.
Полной линковки образа в этой строке нет; Flash/RAM по ней не измеряются.

<!-- feature26-5.5.6-TCP-ws -->
### 2026-10-26 — compile-only 5.5.6-TCP-ws

```ini
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_WEBSOCKET=y
# CONFIG_NAWSERVER_SSE is not set
```
Результат: PASS, скомпилировано объектов NaWServer/потребителя: 25.
Полной линковки образа в этой строке нет; Flash/RAM по ней не измеряются.

<!-- backend26-1-332AF129FDD9 -->
### 2026-10-26 09:50:15 +05:00 — NaWServer_minimal, esp32c3, SDK 5.5.6, профиль 1

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/1`; SDKCONFIG SHA256-prefix `332AF129FDD9`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **942464 байт**; linker total_size: **942108 байт**; статические data+bss: **48372 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 709580 | 0 | 0 |
| Flash Data | 136908 | 0 | 0 |
| DRAM | 131644 | 321296 | 189652 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-5-A7EEB7B478AC -->
### 2026-10-26 09:50:16 +05:00 — NaWServer_showcase, esp32c3, SDK 5.5.6, профиль 5

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/5`; SDKCONFIG SHA256-prefix `A7EEB7B478AC`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1092176 байт**; linker total_size: **1091816 байт**; статические data+bss: **63572 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 740002 | 0 | 0 |
| Flash Data | 236324 | 0 | 0 |
| DRAM | 161710 | 321296 | 159586 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-9-69C1B3D37DDA -->
### 2026-10-26 09:50:17 +05:00 — NaWServer_compile_check, esp32c6, SDK 5.5.6, профиль 9

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/9`; SDKCONFIG SHA256-prefix `69C1B3D37DDA`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **365232 байт**; linker total_size: **364892 байт**; статические data+bss: **12168 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 308708 | 0 | 0 |
| DIRAM | 62264 | 452112 | 389848 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-14-B485F549AD46 -->
### 2026-10-26 09:58:26 +05:00 — NaWServer_minimal, esp32, SDK 5.5.6, профиль 14

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/14`; SDKCONFIG SHA256-prefix `B485F549AD46`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **913008 байт**; linker total_size: **912889 байт**; статические data+bss: **52976 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 649302 | 0 | 0 |
| Flash Data | 148040 | 0 | 0 |
| IRAM | 98259 | 131072 | 32813 |
| DRAM | 52976 | 180736 | 127760 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-10-F5DE70B1709E -->
### 2026-10-26 10:05:30 +05:00 — NaWServer_compile_check, esp32, SDK 5.5.6, профиль 10

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/10`; SDKCONFIG SHA256-prefix `F5DE70B1709E`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **347536 байт**; linker total_size: **347421 байт**; статические data+bss: **14468 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 214118 | 0 | 0 |
| Flash Data | 68780 | 0 | 0 |
| IRAM | 54183 | 131072 | 76889 |
| DRAM | 14468 | 180736 | 166268 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-2-A885D2BEF345 -->
### 2026-10-26 10:05:32 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 2

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/2`; SDKCONFIG SHA256-prefix `A885D2BEF345`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **983280 байт**; linker total_size: **982912 байт**; статические data+bss: **50224 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 742412 | 0 | 0 |
| Flash Data | 154512 | 0 | 0 |
| DRAM | 123056 | 321296 | 198240 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-6-F80C0BA149D7 -->
### 2026-10-26 10:05:33 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 6

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/6`; SDKCONFIG SHA256-prefix `F80C0BA149D7`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1131952 байт**; linker total_size: **1131576 байт**; статические data+bss: **64920 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 774892 | 0 | 0 |
| Flash Data | 253876 | 0 | 0 |
| DRAM | 150132 | 321296 | 171164 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-15-7AFF617C7636 -->
### 2026-10-26 10:09:25 +05:00 — NaWServer_minimal, esp32s2, SDK 5.5.6, профиль 15

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/15`; SDKCONFIG SHA256-prefix `7AFF617C7636`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **880544 байт**; linker total_size: **880434 байт**; статические data+bss: **51368 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 625194 | 0 | 0 |
| Flash Data | 148920 | 0 | 0 |
| DIRAM | 142160 | 172032 | 29872 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-19-4CBA18D4AEFE -->
### 2026-10-26 10:09:26 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 19

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/19`; SDKCONFIG SHA256-prefix `4CBA18D4AEFE`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1391024 байт**; linker total_size: **1390901 байт**; статические data+bss: **74728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 892922 | 0 | 0 |
| Flash Data | 350560 | 0 | 0 |
| DIRAM | 178243 | 341760 | 163517 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-20-2E0D96F4F118 -->
### 2026-10-26 10:09:27 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 20

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/20`; SDKCONFIG SHA256-prefix `2E0D96F4F118`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1188176 байт**; linker total_size: **1187806 байт**; статические data+bss: **72464 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1039814 | 0 | 0 |
| DIRAM | 198636 | 452112 | 253476 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-11-246843EE2A4B -->
### 2026-10-26 10:16:25 +05:00 — NaWServer_compile_check, esp32s2, SDK 5.5.6, профиль 11

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/11`; SDKCONFIG SHA256-prefix `246843EE2A4B`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **347056 байт**; linker total_size: **342226 байт**; статические data+bss: **12400 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 215902 | 0 | 0 |
| Flash Data | 68260 | 0 | 0 |
| DIRAM | 62216 | 172032 | 109816 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-3-088BCCEFC9E1 -->
### 2026-10-26 10:16:26 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 3

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/3`; SDKCONFIG SHA256-prefix `088BCCEFC9E1`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **958576 байт**; linker total_size: **958198 байт**; статические data+bss: **50248 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 728028 | 0 | 0 |
| Flash Data | 144228 | 0 | 0 |
| DRAM | 123034 | 321296 | 198262 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-7-5C61D7BE3EC0 -->
### 2026-10-26 10:16:27 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 7

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/7`; SDKCONFIG SHA256-prefix `5C61D7BE3EC0`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1106976 байт**; linker total_size: **1106602 байт**; статические data+bss: **64952 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 760216 | 0 | 0 |
| Flash Data | 243616 | 0 | 0 |
| DRAM | 150118 | 321296 | 171178 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-12-6345AFD0D258 -->
### 2026-10-26 10:24:57 +05:00 — NaWServer_compile_check, esp32s3, SDK 5.5.6, профиль 12

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/12`; SDKCONFIG SHA256-prefix `6345AFD0D258`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **381040 байт**; linker total_size: **380921 байт**; статические data+bss: **16348 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 234570 | 0 | 0 |
| Flash Data | 73880 | 0 | 0 |
| DIRAM | 60247 | 341760 | 281513 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-16-657A14CE2A91 -->
### 2026-10-26 10:24:58 +05:00 — NaWServer_minimal, esp32s3, SDK 5.5.6, профиль 16

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/16`; SDKCONFIG SHA256-prefix `657A14CE2A91`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **901200 байт**; linker total_size: **901077 байт**; статические data+bss: **53872 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 640542 | 0 | 0 |
| Flash Data | 149812 | 0 | 0 |
| DIRAM | 128451 | 341760 | 213309 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-24-8331D6728746 -->
### 2026-10-26 10:25:00 +05:00 — NaWServer_compile_check, esp32c3, SDK 6.1.0, профиль 24

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/24`; SDKCONFIG SHA256-prefix `8331D6728746`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **370480 байт**; linker total_size: **353558 байт**; статические data+bss: **13052 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 239328 | 0 | 0 |
| Flash Data | 67032 | 0 | 0 |
| DRAM | 53838 | 321296 | 267458 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-17-F61BAC13D1CE -->
### 2026-10-26 10:35:40 +05:00 — NaWServer_minimal, esp32c6, SDK 5.5.6, профиль 17

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/17`; SDKCONFIG SHA256-prefix `F61BAC13D1CE`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1023424 байт**; linker total_size: **1023074 байт**; статические data+bss: **54332 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 898464 | 0 | 0 |
| DIRAM | 165018 | 452112 | 287094 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-18-B35BA43BFDD9 -->
### 2026-10-26 10:35:40 +05:00 — NaWServer_sse_example, esp32c3, SDK 5.5.6, профиль 18

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/18`; SDKCONFIG SHA256-prefix `B35BA43BFDD9`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **895584 байт**; linker total_size: **895234 байт**; статические data+bss: **43812 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 680780 | 0 | 0 |
| DRAM | 127046 | 321296 | 194250 |
| Flash Data | 118824 | 0 | 0 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-21-A579366DB300 -->
### 2026-10-26 10:35:41 +05:00 — NaWServer_sse_lab, esp32c3, SDK 5.5.6, профиль 21

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/21`; SDKCONFIG SHA256-prefix `A579366DB300`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **375424 байт**; linker total_size: **362926 байт**; статические data+bss: **35524 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 244270 | 0 | 0 |
| DRAM | 77980 | 321296 | 243316 |
| Flash Data | 69676 | 0 | 0 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-22-729FB4E4FCAC -->
### 2026-10-26 10:35:41 +05:00 — NaWServer_sse_integration, esp32c3, SDK 5.5.6, профиль 22

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/22`; SDKCONFIG SHA256-prefix `729FB4E4FCAC`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **839472 байт**; linker total_size: **839114 байт**; статические data+bss: **57700 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 626160 | 0 | 0 |
| DRAM | 132542 | 321296 | 188754 |
| Flash Data | 125940 | 0 | 0 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-23-EE0F6FC505FB -->
### 2026-10-26 10:35:41 +05:00 — url_decode_bench, esp32c3, SDK 5.5.6, профиль 23

Исходники: `632dafd` + переход конфигураций SDK; build `local/backend26-idf/23`; SDKCONFIG SHA256-prefix `EE0F6FC505FB`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_SSE=y
```

BIN: **225984 байт**; linker total_size: **225652 байт**; статические data+bss: **13684 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Data | 101436 | 0 | 0 |
| Flash Code | 78050 | 0 | 0 |
| DRAM | 54466 | 321296 | 266830 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### Runtime S3 — 2026-10-26T05:37:43.609Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T05:37:43.609Z",
  "checks": 2,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 112365
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "09:56:40",
    "reset_reason": 11
  },
  "source": "device",
  "status": "FAIL",
  "error": "Error: WS connection error"
}
```

### Runtime S3 — 2026-10-26T05:39:39.749Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T05:39:39.749Z",
  "checks": 5,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 228465,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 68447,
          "min_free_bytes": 25572,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120512,
          "free_bytes": 5580400,
          "min_free_bytes": 5580228,
          "largest_block_bytes": 5505024
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 229165,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 67091,
          "min_free_bytes": 25572,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120512,
          "free_bytes": 5580400,
          "min_free_bytes": 5580228,
          "largest_block_bytes": 5505024
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "09:56:40",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [
    "Error: WS connection error: "
  ],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-2e1357c6-e720f38c",
    "seq": 2291,
    "captured_ms": 229066,
    "cpu_busy_pct": 3.8,
    "free_bytes": 70855,
    "task_count": 16
  },
  "status": "PARTIAL"
}
```

### Runtime C6 — 2026-10-26T05:43:35.158Z (UTC)

Подключённая плата, http://192.0.2.1. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "c6",
  "base": "http://192.0.2.1",
  "measured_utc": "2026-10-26T05:43:35.158Z",
  "checks": 5,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 27079,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339244,
          "free_bytes": 230296,
          "min_free_bytes": 223484,
          "largest_block_bytes": 208896
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 27579,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339244,
          "free_bytes": 228676,
          "min_free_bytes": 223472,
          "largest_block_bytes": 208896
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-C6",
    "revision_major": 0,
    "revision_minor": 1,
    "cores": 1,
    "psram": false,
    "features": 82
  },
  "firmware": {
    "name": "NaWServer_c6_lcd147",
    "version": "0.1.0-c6-demo",
    "idf_version": "5.5.6",
    "board_label": "Waveshare C6 LCD 1.47",
    "build_date": "Oct 26 2026",
    "build_time": "09:59:30",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [
    "Error: WS connection error: "
  ],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "c6-1cea5752-378d7adb",
    "seq": 271,
    "captured_ms": 27480,
    "cpu_busy_pct": 4.5,
    "free_bytes": 230748,
    "task_count": 12
  },
  "status": "PARTIAL"
}
```

## 2026-10-26 — исправление WebSocket после 101 на C6/S3

Ветка `codex/http-backend-adapters`, базовый commit `632dafd`, локальные исправления. Окружение и установленные пакеты не обновлялись. Обе платы используют native ESP-IDF HTTPD, SDK 5.5.6; это не общий TCP backend.

Причина: в установленной IDF 5.5.6 `httpd_uri.c` после успешного WS upgrade возвращает управление без вызова обычного URI handler. Создание клиентской сессии требует `ws_post_handshake_cb`. NaWServer уже регистрировал callback при включённой опции, но профили C6/S3 не включали `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT`, а CMake требовал её только для major >= 6. Поэтому HTTP 101 не означал работоспособный обмен.

Изменения: включена опция в native demo/test defaults; CMake определяет наличие API по заголовку выбранного SDK и запрещает ошибочную native WS конфигурацию независимо от major. TCP и HTTP-only профили не требуют этой опции. INFO C6/S3 получает `firmware.http_backend=idf_httpd`; существующий frontend показывает его в INFO → «Сборка» → «HTTP / WS / SSE» как `ESP-IDF HTTPD`.

Регрессия: новый CMake сценарий SDK5 с backport API и отключённым hook сначала падал (ошибочная конфигурация принималась); после исправления проходит. Проверены legacy SDK5 без API, SDK5 с/без hook, SDK6 с/без hook, HTTP-only и TCP. Windows CTest: 46/46; frontend Node tests: 12/12. Обе device frontend сборки Vue/TypeScript и упаковка локальных ресурсов прошли.

### Runtime C6 — 2026-10-26T06:02:39.973Z (UTC)

Подключённая плата, http://192.0.2.1. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "c6",
  "base": "http://192.0.2.1",
  "measured_utc": "2026-10-26T06:02:39.973Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 11044,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339244,
          "free_bytes": 229468,
          "min_free_bytes": 224720,
          "largest_block_bytes": 208896
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 13844,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339244,
          "free_bytes": 225328,
          "min_free_bytes": 217748,
          "largest_block_bytes": 204800
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-C6",
    "revision_major": 0,
    "revision_minor": 1,
    "cores": 1,
    "psram": false,
    "features": 82
  },
  "firmware": {
    "name": "NaWServer_c6_lcd147",
    "version": "0.1.0-c6-demo",
    "idf_version": "5.5.6",
    "http_backend": "idf_httpd",
    "board_label": "Waveshare C6 LCD 1.47",
    "build_date": "Oct 26 2026",
    "build_time": "10:59:25",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "c6-aa10d5dc-4e3ed8af",
    "seq": 131,
    "captured_ms": 13545,
    "cpu_busy_pct": 4.8,
    "free_bytes": 227384,
    "task_count": 12
  },
  "status": "PASS"
}
```

### Runtime S3 — 2026-10-26T06:03:24.066Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:03:24.066Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime S3 — 2026-10-26T06:03:40.015Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:03:40.015Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```
<!-- backend26-19-D4114FF7208D -->
### 2026-10-26 11:03:45 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 19

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/19`; SDKCONFIG SHA256-prefix `D4114FF7208D`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1391360 байт**; linker total_size: **1391237 байт**; статические data+bss: **74728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 893034 | 0 | 0 |
| Flash Data | 350784 | 0 | 0 |
| DIRAM | 178243 | 341760 | 163517 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-20-8FB4FF66AAF0 -->
### 2026-10-26 11:03:46 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 20

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/20`; SDKCONFIG SHA256-prefix `8FB4FF66AAF0`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1188400 байт**; linker total_size: **1188038 байт**; статические data+bss: **72464 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1040046 | 0 | 0 |
| DIRAM | 198636 | 452112 | 253476 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### Runtime S3 — 2026-10-26T06:04:07.194Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:04:07.194Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime S3 — 2026-10-26T06:05:06.404Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:05:06.404Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime S3 — 2026-10-26T06:08:34.497Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:08:34.497Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime S3 — 2026-10-26T06:09:41.452Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:09:41.452Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime S3 — 2026-10-26T06:11:51.169Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:11:51.169Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 30965,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 72903,
          "min_free_bytes": 36464,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120288,
          "free_bytes": 5580176,
          "min_free_bytes": 5580004,
          "largest_block_bytes": 5505024
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 39365,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 64619,
          "min_free_bytes": 28740,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120288,
          "free_bytes": 5580176,
          "min_free_bytes": 5580004,
          "largest_block_bytes": 5505024
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "http_backend": "idf_httpd",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "10:59:26",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-4ee4bafd-2c3e34fd",
    "seq": 391,
    "captured_ms": 39066,
    "cpu_busy_pct": 3.3,
    "free_bytes": 69419,
    "task_count": 16
  },
  "status": "PASS"
}
```

### Runtime S3 — 2026-10-26T06:13:17.057Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:13:17.057Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime C6 — 2026-10-26T06:14:00.172Z (UTC)

Подключённая плата, http://192.0.2.1. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "c6",
  "base": "http://192.0.2.1",
  "measured_utc": "2026-10-26T06:14:00.172Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 691245,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339244,
          "free_bytes": 229560,
          "min_free_bytes": 207516,
          "largest_block_bytes": 208896
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 693744,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339244,
          "free_bytes": 225380,
          "min_free_bytes": 207516,
          "largest_block_bytes": 204800
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-C6",
    "revision_major": 0,
    "revision_minor": 1,
    "cores": 1,
    "psram": false,
    "features": 82
  },
  "firmware": {
    "name": "NaWServer_c6_lcd147",
    "version": "0.1.0-c6-demo",
    "idf_version": "5.5.6",
    "http_backend": "idf_httpd",
    "board_label": "Waveshare C6 LCD 1.47",
    "build_date": "Oct 26 2026",
    "build_time": "10:59:25",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "c6-aa10d5dc-4e3ed8af",
    "seq": 6931,
    "captured_ms": 693545,
    "cpu_busy_pct": 3.9,
    "free_bytes": 227420,
    "task_count": 12
  },
  "status": "PASS"
}
```

### Runtime S3 — 2026-10-26T06:14:48.220Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:14:48.220Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 27765,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 72871,
          "min_free_bytes": 36432,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120288,
          "free_bytes": 5580176,
          "min_free_bytes": 5580004,
          "largest_block_bytes": 5505024
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 31265,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 66303,
          "min_free_bytes": 29596,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120288,
          "free_bytes": 5580176,
          "min_free_bytes": 5580004,
          "largest_block_bytes": 5505024
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "http_backend": "idf_httpd",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "10:59:26",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-a94b3e1f-83b2f97c",
    "seq": 311,
    "captured_ms": 31066,
    "cpu_busy_pct": 3.8,
    "free_bytes": 69007,
    "task_count": 16
  },
  "status": "PASS"
}
```

### WebSocket: непрерывное соединение 60 секунд на обеих платах

```json
[
  {
    "tag": "c6",
    "base": "http://192.0.2.1",
    "started_utc": "2026-10-26T06:15:24.000Z",
    "echo_sent": 59,
    "echo_received": 59,
    "summaries": 60,
    "status": "PASS",
    "finished_utc": "2026-10-26T06:16:24.073Z"
  },
  {
    "tag": "s3",
    "base": "http://192.0.2.2",
    "started_utc": "2026-10-26T06:15:24.016Z",
    "echo_sent": 59,
    "echo_received": 3,
    "summaries": 4,
    "status": "FAIL",
    "error": "Error: Missing echo/telemetry",
    "finished_utc": "2026-10-26T06:16:28.078Z"
  }
]
```

### Итог аппаратной проверки WS исправления

Окончательные образы собраны с IDF 5.5.6; C3 compile-check с IDF 6.1 также пересобран успешно. Изменений SDK/драйверов/установленных зависимостей не было. Временные диагностические строки S3 `main.cpp` удалены, окончательный образ повторно собран и прошит. Записывались только bootloader, partition table и factory app в проверенных диапазонах; NVS не стиралась. Обычная проверка записи esptool прошла, отдельных backup/readback/hash-проходов не выполнялось.

| Плата | Порт / MAC | Адрес | Финальный BIN, байт | data+bss, байт | Функциональный прогон |
|---|---|---|---:|---:|---|
| C6 | COM13 / DEVICE-MAC | http://192.0.2.1/ | 1188400 | 72464 | PASS 27, повторный PASS 27 |
| S3 | COM12 / DEVICE-MAC | http://192.0.2.2/ | 1391360 | 74728 | PASS 27, после повторного запуска PASS 27 |

Каждый прогон: INFO и summary в CBOR v2; source=device; INFO backend=idf_httpd; 20 последовательных WS echo/reconnect (чередование UTF-8 с переводом строки и binary `[65,0,66,255]`); диагностический SSE source=device; дополнительный WS echo и HTTP summary при ещё открытом SSE; API списка задач; итоговый снимок памяти. Порог проверки — точный echo.response/request_id/payload, а не только HTTP 101.

Обе платы дополнительно прошли независимый клиент Go с прямым RFC6455 handshake, замаскированным binary frame и последующим отдельным декодированием CBOR echo.response с текстом «Проверка». В браузере на обеих платах подтверждены INFO → «Сборка» → «HTTP / WS / SSE» = ESP-IDF HTTPD, версия SDK 5.5.6, все каналы HTTP/WS/SSE онлайн. Данные относятся к физическим устройствам, не к localhost-симулятору.

После включения hook и обновления встроенных assets размер BIN относительно предыдущих образов увеличился на 224 байта C6 и 336 байт S3; статические data+bss не изменились. Точные конфигурации, linker map области и runtime JSON находятся выше. Снимок после первого успешного прогона S3: internal free=64619, min=28740, largest=31744; PSRAM free=5580176 байт. C6 после повторного прогона: internal free=225380, min=207516, largest=204800 байт. Снимки не доказывают отсутствие утечек.

Ограничение: S3 некоторое время не отвечала после прошивки; после первого успешного прогона и открытия UI снова наблюдалась недоступность TCP/HTTP и отсутствие ARP-ответа. После повторного аппаратного сброса восстановилась по тому же IP и повторно прошла все 27 проверок и независимый raw WS echo. Причина этого эпизода доступности не установлена; это не воспроизведение прежнего 101-without-session, так как недоступен уже сам TCP/IP. Несколько ранних попыток и ошибки сохранены выше, общий статус долгой сетевой стабильности не объявляется PASS. При браузерном наблюдении минимум стека httpd S3 был 228 байт; переполнения этим измерением не доказано, нагрузочный запас не подтверждён.

### Runtime S3 — 2026-10-26T06:18:45.285Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:18:45.285Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime S3 — 2026-10-26T06:19:13.061Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:19:13.061Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime S3 — 2026-10-26T06:20:00.619Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:20:00.619Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Уточнение после длительного WS прогона

Минутный непрерывный прогон (JSON выше) расширил короткую проверку: C6 PASS, 59/59 echo и 60 summary. S3 FAIL: 3/59 echo и 4 summary, затем недоступны HTTP/TCP/ARP. Поэтому короткие PASS 27 не означают принятую длительную работу S3.

Проверяется гипотеза недостаточного запаса стека: временный native HTTPD stack 8192 вместо 6144 байт. На момент проверки после прошивки S3 ещё не отвечала в LAN, поэтому эффект увеличения стека не установлен. Эти изменения пока диагностические. Для получения журнала после LCD init временно сохранён EXIO5/USB selector high (`set_backlight` 0x3e/0x3a); это также не окончательная правка. Попытка JTAG через уже установленную OpenOCD не смогла открыть USB-интерфейс (libusb/serial selection); новых драйверов и пакетов не устанавливалось.

### Диагностический USB selector — требуется возврат обычной прошивки

Пробный EXIO5 high после LCD init отключил USB-интерфейс S3: COM12 исчез из списка устройств. Это побочный эффект диагностической правки, не исправление. `board.cpp` и backend stack уже восстановлены в исходниках, обычный образ 6144 / normal USB собран успешно, но его запись остановлена до идентификации платы, поскольку USB недоступен. Владельцу предложено подключить S3 с BOOT для восстановления; до успешной обратной прошивки на плате остаётся диагностический образ (stack8192, EXIO5 high). Сеть текущего диагностического образа ответила: boot=s3-13e24087-3fbdc195, captured_ms=178365. Не считать диагностический образ окончательным.

### WebSocket: непрерывное соединение 60 секунд на обеих платах

```json
[
  {
    "tag": "s3",
    "base": "http://192.0.2.2",
    "started_utc": "2026-10-26T06:25:19.773Z",
    "echo_sent": 59,
    "echo_received": 59,
    "summaries": 60,
    "status": "PASS",
    "finished_utc": "2026-10-26T06:26:27.182Z"
  }
]
```

### Диагностический длительный прогон S3 — PASS, причина не изолирована

Образ stack8192 + временный USB selector high прошёл минутный WS тест: 59/59 echo, 60 summary, тот же boot `s3-13e24087-3fbdc195`; HTTP summary после прогона отвечает, uptime около 280 секунд. Поскольку изменялись две переменные, нельзя приписывать улучшение одному увеличению стека. USB интерфейс отключён этой пробой и требует возврата обычного образа через BOOT. Обычный образ stack6144 с восстановленным `board.cpp` собран и сохранён также в ignored `local/wsfix-s3-normal6144/` для восстановления. Требуется отдельно проверить stack8192 при обычном USB selector после возврата порта. Полная приёмка S3 остаётся открытой.

### Состояние перед ожиданием восстановления USB S3

Подготовлены два обычных USB образа в ignored `local/wsfix-s3-normal6144/` и `local/wsfix-s3-normal8192/`, различающиеся только размером стека native HTTPD. Сборки успешны. Эти варианты ещё не прошиты после диагностического USB selector образа: COM12 отсутствует. В исходниках возвращены обычные значения: stack6144, исходный `board.cpp`, исходный `main.cpp`; диагностических изменений в них не оставлено. В локальном build/19 сейчас лежит пробный normal8192, для обычного восстановления использовать сохранённый normal6144, а не предполагать, что build/19 совпадает с восстановленным исходником. На устройстве S3 пока остаётся stack8192 + USB selector high, boot=s3-13e24087-3fbdc195. Требуется BOOT при подключении для возврата USB и отдельная проверка normal8192; при отсутствии подтверждения вернуть normal6144. C6 остаётся на исправленном обычном образе, прошедшем минутный тест. Окружение не обновлялось, commit/push не выполнялись.

### 2026-10-26 — восстановление S3 через независимый UART

После сообщения владельца об отключении питания и пустом экране Windows по-прежнему не видит USB S3 (не только COM12: отсутствует present VID303A с её serial). Повторные требования обесточивания не дают новой информации. Сверена сохранённая схема V1.2 `examples/touch_lcd7/docs/board/ESP32-S3-Touch-LCD-7-Sch.pdf` (полная страница и крупные фрагменты): U9 FSUSB42UMX по USB_SEL переключает ESP_USB_P/N между native USB (HSD1) и CANRX/CANTX (HSD2). Временный EXIO5 high действительно выбрал CAN, а не сохранение USB; исходная диагностическая предпосылка была ошибочной. На схеме есть отдельный Type_C1 USB to UART с CH343P и цепью DTR/RTS auto-download; он не проходит через U9. Владельцу предложено переставить кабель в UART для восстановления, новых драйверов/пакетов не устанавливается.

### Runtime S3 — 2026-10-26T06:44:22.970Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:44:22.970Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Восстановление через CH343 и точная причина offline

UART S3 появился как CH343 COM11 (`VID_1A86&PID_55D3`), драйверы не устанавливались. Esptool свежо подтвердил ESP32-S3 и MAC DEVICE-MAC; через независимый UART успешно записан обычный образ normal6144 с прежним USB selector (0x1e/0x1a). Запись проверена штатным esptool, NVS не стиралась. Диагностического EXIO5 high на устройстве больше нет. Встроенный USB-разъём отдельно не проверялся переподключением, связь для восстановления и журналов идёт через UART.

UART capture `local/wsfix-s3-uart-boot.log`: app_main возвращается, Wi-Fi driver стартует, GT911 определяется, затем `Wi-Fi reconnect limit reached` на 27546 ms. Добавлен только диагностический вывод reason/retries в S3 main.cpp (без изменения retry policy). Следующий UART capture `local/wsfix-s3-wifi-reasons.log`: 11 событий NO_AP_FOUND (201), retries=0..10, от 3479 до 27549 ms; далее reconnect limit. Признаков panic/stack canary этим capture не получено. Таким образом конкретный offline на этом запуске вызван отсутствием найденной AP, а не доказанным зависанием HTTPD.

Секреты не выводились: буквальные SSID и PASSWORD S3/C6 совпадают. ПК подключён к <SSID>, BSSID DEVICE-MAC; C6 остаётся на <SSID> (последний RSSI -79 dBm, прежний boot c6-aa10d5dc-4e3ed8af). В исходниках C6 уже отключает Wi-Fi power save, S3 оставляла SDK default WIFI_PS_MIN_MODEM (подтверждено esp_wifi.h установленного SDK). Подготовлена отдельная проверка S3 с WIFI_PS_NONE; stack6144 и обычный USB не изменены. Результат будет записан ниже; сам факт этой гипотезы не доказывает причину NO_AP_FOUND.

### Runtime S3 — 2026-10-26T06:51:20.076Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:51:20.076Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 51665,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 70667,
          "min_free_bytes": 33976,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120100,
          "free_bytes": 5579988,
          "min_free_bytes": 5579816,
          "largest_block_bytes": 5505024
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 54265,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 64099,
          "min_free_bytes": 27380,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120100,
          "free_bytes": 5579988,
          "min_free_bytes": 5579816,
          "largest_block_bytes": 5505024
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "http_backend": "idf_httpd",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "10:59:26",
    "reset_reason": 1
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-447bfcf5-177543a3",
    "seq": 541,
    "captured_ms": 54066,
    "cpu_busy_pct": 3.5,
    "free_bytes": 66791,
    "task_count": 16
  },
  "status": "PASS"
}
```

### WebSocket: непрерывное соединение (длительность и платы в JSON)

```json
[
  {
    "tag": "s3",
    "base": "http://192.0.2.2",
    "started_utc": "2026-10-26T06:52:12.130Z",
    "duration_seconds": 180,
    "echo_sent": 178,
    "echo_received": 178,
    "summaries": 180,
    "status": "PASS",
    "finished_utc": "2026-10-26T06:55:12.184Z"
  }
]
```

### Контроль фактической конфигурации S3 и повторная сборка backend

При проверке артефактов выявлена ошибка инкрементальной сборки: Copy-Item при возврате backend исходника сохранил старую дату 10:56:26, объект esp_idf.cpp.obj был новее (11:27:58) и оставался от пробного stack8192. Поэтому название сохранённой папки normal6144 само по себе не подтверждало стек6144, а ряд предыдущих заявлений о возврате именно этого размера стека был преждевременным. Обычный USB selector восстановлен независимо от этого и подтверждён записью исходного board.cpp.

Исходник backend содержит config.stack_size=6144. Его timestamp обновлён явно, выполнена новая SDK сборка; лог содержит отдельную компиляцию esp_idf.cpp.obj [8/21] и пересоздание libNaWServer.a. Старый snapshot local/wsfix-s3-normal6144 помечен INVALID и не предназначен для прошивки. Текущий длинный тест WIFI_PS_NONE пока относится к ранее собранному объекту stack8192; окончательная проверка должна выполняться на новом образе после принудительной пересборки. Это уточнение важно для сравнения RAM и причин улучшения.

### Runtime S3 — 2026-10-26T06:56:52.375Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T06:56:52.375Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 23965,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 72763,
          "min_free_bytes": 36320,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120100,
          "free_bytes": 5579988,
          "min_free_bytes": 5579816,
          "largest_block_bytes": 5505024
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 27265,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 66163,
          "min_free_bytes": 29448,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120100,
          "free_bytes": 5579988,
          "min_free_bytes": 5579816,
          "largest_block_bytes": 5505024
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "http_backend": "idf_httpd",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "10:59:26",
    "reset_reason": 1
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-500931da-27af2cac",
    "seq": 271,
    "captured_ms": 27066,
    "cpu_busy_pct": 3.5,
    "free_bytes": 68991,
    "task_count": 16
  },
  "status": "PASS"
}
```

### Окончательный образ S3 после восстановления и точной пересборки

BIN: **1391888 байт**; linker total_size: **1391773 байт**; static data+bss: **74728 байт**. SDKCONFIG SHA256-prefix: `01FCF21B6E07`; полный конфиг профиля 19 приведён выше. Отдельных flash readback/hash-проверок не выполнялось.

Native HTTPD stack=6144 (исходник и объект backend пересобраны); USB selector 0x1e/0x1a; post-handshake callback=y; Wifi power-save=NONE (прямой вызов demo, как на C6, не настройка общей библиотеки). Модемное энергосбережение в питаемой LCD-демке отключено; потребление тока не измерялось. Лимит повторных подключений не менялся. Добавлен reason/retries в лог для будущей диагностики.

```json
{
  "sdk": "5.5.6",
  "target": "esp32s3",
  "http_backend": "idf_httpd",
  "httpd_stack_bytes": 6144,
  "wifi_power_save": "WIFI_PS_NONE",
  "usb_selector": "normal 0x1e/0x1a",
  "binary_sha256": "55836C910E4CDD2199B76C4F23A672855F33D6C95D671359C49DF7567460DBB2",
  "backend_source_sha256": "092800786B5393DACCA5829D162855271ED6439FB5260E003BAC454870470A7E",
  "main_source_sha256": "F0E58C6F65C10A9C98F99EF41DA7FD9B67D4F2806A4DA2C894A378DC3B4235E6",
  "recorded_utc": "2026-10-26T06:56:01.0835408Z"
}

```

| Linker region | Used, B | Total, B | Free, B |
|---|---:|---:|---:|
| Flash Code | 893382 | 0 | 0 |
| Flash Data | 350972 | 0 | 0 |
| DIRAM | 178243 | 341760 | 163517 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime INFO ELF SHA256 совпал с SHA256 текущего локального ELF. Подтверждены backend=idf_httpd, SDK5.5.6, boot=s3-500931da-27af2cac; index HTML и JS/CSS ресурсы получены HTTP200 во время WS теста. Минимум свободного стека httpd после запросов — 228 байт: это подтверждает небольшой запас, но само по себе не доказывает переполнение. Увеличение стека в окончательную правку не включено.

```json
{
  "measured_utc": "2026-10-26T06:58:19.909Z",
  "stage": "final6144_WiFiPsNone_during_180s_ws",
  "info": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-500931da-27af2cac",
    "chip": {
      "model": "ESP32-S3",
      "revision_major": 0,
      "revision_minor": 2,
      "cores": 2,
      "psram": true,
      "features": 18
    },
    "firmware": {
      "name": "NaWServer_touch_lcd7",
      "version": "0.1.0-touch-lcd7",
      "idf_version": "5.5.6",
      "http_backend": "idf_httpd",
      "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
      "build_date": "Oct 26 2026",
      "build_time": "10:59:26",
      "reset_reason": 1
    },
    "capabilities": {
      "tasks": {
        "supported": true,
        "origin": "device",
        "device_status": "implemented_unverified"
      },
      "memory": {
        "supported": true,
        "origin": "device",
        "device_status": "implemented_unverified"
      },
      "experiments": {
        "supported": true,
        "origin": "device",
        "device_status": "implemented_unverified"
      },
      "led": {
        "supported": false,
        "origin": "device",
        "device_status": "unsupported",
        "reason": "board_led_not_confirmed"
      },
      "strip": {
        "supported": true,
        "origin": "device",
        "device_status": "implemented_unverified"
      },
      "connections": {
        "supported": false,
        "origin": "device",
        "device_status": "unsupported",
        "reason": "session_details_not_exposed"
      },
      "simulator_control": {
        "supported": false,
        "origin": "device",
        "device_status": "unsupported",
        "reason": "device"
      },
      "sse": {
        "supported": true,
        "origin": "device",
        "device_status": "implemented_unverified"
      },
      "authenticated_ws": {
        "supported": false,
        "origin": "device",
        "device_status": "unsupported",
        "reason": "pre_handshake_auth_unavailable"
      }
    },
    "limits": {
      "frame_bytes": 4096,
      "body_bytes": 4096,
      "task_page_max": 8,
      "max_ws_clients": 4
    },
    "details": {
      "hardware": {
        "chip_model": "ESP32-S3",
        "architecture": "Xtensa LX7",
        "cpu_mhz": 240,
        "crystal_mhz": 40,
        "apb_mhz": 80,
        "revision_major": 0,
        "revision_minor": 2,
        "cores": 2,
        "wifi_24ghz": true,
        "wifi_6": false,
        "bluetooth_le": true,
        "bluetooth_classic": false,
        "ieee802154": false,
        "usb_serial_jtag": true,
        "hp_sram_bytes": 524288,
        "lp_sram_bytes": 16384
      },
      "flash_memory": {
        "flash_bytes": 16777216,
        "flash_jedec_id": "0xC84018",
        "embedded_flash": false,
        "heap_internal_bytes": 283955,
        "heap_dma_bytes": 275787,
        "psram_bytes": 7120100,
        "app_partition": "factory",
        "app_partition_bytes": 5242880
      },
      "mac_addresses": {
        "factory_mac": "DEVICE-MAC",
        "custom_mac": null,
        "base_mac": "DEVICE-MAC",
        "wifi_sta_mac": "DEVICE-MAC",
        "wifi_ap_mac": "DEVICE-MAC",
        "bluetooth_mac": "DEVICE-MAC",
        "ethernet_mac": "DEVICE-MAC",
        "ieee802154_eui64": null
      },
      "network": {
        "wifi_connected": true,
        "wifi_mode": "STA",
        "ipv4": "192.0.2.2",
        "netmask": "255.255.240.0",
        "gateway": "192.0.2.4",
        "ssid": "<SSID>",
        "ap_bssid": "DEVICE-MAC",
        "channel": 6,
        "rssi_dbm": -71,
        "wifi_auth_mode": 7
      },
      "board": {
        "lcd_interface": "RGB565",
        "lcd_resolution": "800 x 480",
        "lcd_ready": true,
        "backlight_enabled": true,
        "touch": true,
        "touch_controller": "GT911",
        "renderer": "LVGL 8.3.11",
        "framebuffers": 2,
        "rgb_count": 10,
        "rgb_gpio": 6,
        "rgb_order": "GRB",
        "rgb_driver": "RMT",
        "rgb_ready": true,
        "ble_started": false
      },
      "build": {
        "project": "NaWServer_touch_lcd7",
        "version": "0.1.0-touch-lcd7",
        "idf_version": "5.5.6",
        "build_date": "Oct 26 2026",
        "build_time": "10:59:26",
        "secure_version": 0,
        "elf_sha256": "1722470e44683f7e9be2da7f7b4803f78ce23e0292f106c3d89589d944a8775f",
        "reset_reason": 1
      }
    }
  },
  "summary": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-500931da-27af2cac",
    "seq": 1115,
    "captured_ms": 111465,
    "data": {
      "uptime_ms": 111465,
      "sample_interval_ms": 1000,
      "cpu_busy_pct": 3.8869222615547705,
      "core_busy_pct": [
        7.274854502909944,
        0.4989900201995967
      ],
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 283955,
          "free_bytes": 67499,
          "min_free_bytes": 26744,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7120100,
          "free_bytes": 5579988,
          "min_free_bytes": 5579816,
          "largest_block_bytes": 5505024
        }
      ],
      "rssi_dbm": -71,
      "task_count": 16,
      "ws_clients": 1,
      "network_status": "connected",
      "counters": {
        "http_requests": 13,
        "ws_rx_frames": 100,
        "ws_tx_frames": 182,
        "ws_rx_bytes": 8252,
        "ws_tx_bytes": 122673,
        "rejected": 8
      },
      "experiment": {
        "running": false,
        "kind": "pi",
        "duration_ms": 5000,
        "on_ms": 200,
        "period_ms": 1000,
        "core": 0,
        "priority": 2,
        "elapsed_ms": 0,
        "progress_pct": 0,
        "pi": 0,
        "iterations": 0,
        "active": false,
        "sample_ms": 100,
        "enabled": false
      },
      "led": {
        "supported": false,
        "enabled": false,
        "period_ms": 1000,
        "duty_pct": 50,
        "gpio": -1,
        "active_low": true,
        "level": false,
        "reason": "board_led_not_confirmed"
      },
      "strip": {
        "driver": "rmt",
        "requested_driver": "rmt",
        "driver_error": "",
        "supported": true,
        "enabled": false,
        "gpio": 6,
        "count": 10,
        "mode": "solid",
        "color": 16752688,
        "brightness_pct": 20,
        "period_ms": 1000,
        "pixels": [
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0
        ]
      },
      "display": {
        "ready": true,
        "width": 800,
        "height": 480,
        "pattern": 0,
        "backlight": true,
        "pressed": false,
        "x": 0,
        "y": 0,
        "touches": 0,
        "touch_errors": 0,
        "renderer": "lvgl-8.3.11",
        "framebuffers": 2,
        "frame_swaps": 2,
        "frame_events": 3250,
        "vsyncs": 3250
      },
      "network": {
        "socket_limit": 24,
        "tcp_pcb_limit": 32,
        "reserved_sockets": 3,
        "httpd_limit": 21,
        "httpd_open": 3,
        "httpd_free": 18,
        "http_reserve": 3,
        "ws_limit": 4,
        "ws_clients": 1,
        "sse_limit": 4,
        "sse_clients": 0,
        "admission_rejected": 0,
        "last_rejection": "none",
        "retry_after_ms": 2000,
        "captured_ms": 111560,
        "ready": true
      }
    }
  },
  "assets": [
    {
      "path": "/assets/index-CriSqE7O.js",
      "status": 200,
      "bytes": 189094
    },
    {
      "path": "/assets/index-D7mmyRPE.css",
      "status": 200,
      "bytes": 37139
    }
  ],
  "httpd_task": [
    {
      "id": "task-15",
      "name": "httpd",
      "state": "blocked",
      "priority": 5,
      "core_affinity": null,
      "stack_min_free_bytes": 228,
      "runtime_ticks": 572356,
      "cpu_pct": 0.21370854834193367
    }
  ]
}
```

### WebSocket: непрерывное соединение (длительность и платы в JSON)

```json
[
  {
    "tag": "s3",
    "base": "http://192.0.2.2",
    "started_utc": "2026-10-26T06:57:00.205Z",
    "duration_seconds": 180,
    "echo_sent": 178,
    "echo_received": 178,
    "summaries": 180,
    "status": "PASS",
    "finished_utc": "2026-10-26T07:00:00.291Z"
  }
]
```

### Финальные снимки после функциональных и длительных WS проверок

```json
[
  {
    "tag": "c6",
    "base": "http://192.0.2.1",
    "measured_utc": "2026-10-26T07:02:05.768Z",
    "info": {
      "v": 2,
      "source": "device",
      "boot_id": "c6-aa10d5dc-4e3ed8af",
      "chip": {
        "model": "ESP32-C6",
        "revision_major": 0,
        "revision_minor": 1,
        "cores": 1,
        "psram": false,
        "features": 82
      },
      "firmware": {
        "name": "NaWServer_c6_lcd147",
        "version": "0.1.0-c6-demo",
        "idf_version": "5.5.6",
        "http_backend": "idf_httpd",
        "board_label": "Waveshare C6 LCD 1.47",
        "build_date": "Oct 26 2026",
        "build_time": "10:59:25",
        "reset_reason": 11
      },
      "capabilities": {
        "tasks": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "memory": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "experiments": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "led": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "connections": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "session_details_not_exposed"
        },
        "simulator_control": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "device"
        },
        "sse": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "authenticated_ws": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "pre_handshake_auth_unavailable"
        }
      },
      "details": {
        "hardware": {
          "chip_model": "ESP32-C6",
          "architecture": "RISC-V",
          "cpu_mhz": 160,
          "crystal_mhz": 40,
          "apb_mhz": 40,
          "revision_major": 0,
          "revision_minor": 1,
          "cores": 1,
          "wifi_24ghz": true,
          "wifi_6": true,
          "bluetooth_le": true,
          "bluetooth_classic": false,
          "ieee802154": true,
          "usb_serial_jtag": true,
          "hp_sram_bytes": 524288,
          "lp_sram_bytes": 16384
        },
        "flash_memory": {
          "flash_bytes": 4194304,
          "flash_jedec_id": "0x204016",
          "embedded_flash": true,
          "heap_internal_bytes": 339244,
          "heap_dma_bytes": 322916,
          "psram_bytes": 0,
          "app_partition": "factory",
          "app_partition_bytes": 3145728
        },
        "mac_addresses": {
          "factory_mac": "DEVICE-MAC",
          "custom_mac": null,
          "base_mac": "DEVICE-MAC",
          "wifi_sta_mac": "DEVICE-MAC",
          "wifi_ap_mac": "DEVICE-MAC",
          "bluetooth_mac": "DEVICE-MAC",
          "ethernet_mac": "DEVICE-MAC",
          "ieee802154_eui64": "DEVICE-MAC:1F:5C"
        },
        "network": {
          "wifi_connected": true,
          "wifi_mode": "STA",
          "ipv4": "192.0.2.1",
          "netmask": "255.255.240.0",
          "gateway": "192.0.2.4",
          "ssid": "<SSID>",
          "ap_bssid": "DEVICE-MAC",
          "channel": 6,
          "rssi_dbm": -77,
          "wifi_auth_mode": 7
        },
        "board": {
          "lcd_controller": "ST7789T",
          "lcd_resolution": "172 x 320",
          "lcd_spi_ready": true,
          "lcd_visual_status": "unverified",
          "backlight_percent": 30,
          "rgb_count": 1,
          "rgb_gpio": 8,
          "rgb_order": "RGB",
          "rgb_driver": "RMT",
          "sd_slot": true,
          "sd_mounted": false,
          "touch": false,
          "ble_started": false
        },
        "build": {
          "project": "NaWServer_c6_lcd147",
          "version": "0.1.0-c6-demo",
          "idf_version": "5.5.6",
          "build_date": "Oct 26 2026",
          "build_time": "10:59:25",
          "secure_version": 0,
          "elf_sha256": "efc03c69b18ea94389b8f87c1b3190e2adbc3d5c316066fb1dac1ec5102eca75",
          "reset_reason": 11
        }
      },
      "limits": {
        "frame_bytes": 4096,
        "body_bytes": 4096,
        "task_page_max": 8,
        "max_ws_clients": 4
      }
    },
    "summary": {
      "v": 2,
      "source": "device",
      "boot_id": "c6-aa10d5dc-4e3ed8af",
      "seq": 35764,
      "captured_ms": 3576844,
      "data": {
        "uptime_ms": 3576844,
        "sample_interval_ms": 1000,
        "cpu_busy_pct": 38.392866068840036,
        "core_busy_pct": [
          38.392866068840036
        ],
        "memory": [
          {
            "id": "internal",
            "label": "Внутренняя RAM",
            "total_bytes": 339244,
            "free_bytes": 229984,
            "min_free_bytes": 207516,
            "largest_block_bytes": 208896
          }
        ],
        "rssi_dbm": -77,
        "task_count": 12,
        "ws_clients": 0,
        "network_status": "connected",
        "counters": {
          "http_requests": 1573,
          "ws_rx_frames": 102,
          "ws_tx_frames": 720,
          "ws_rx_bytes": 8280,
          "ws_tx_bytes": 569981,
          "rejected": 38
        },
        "experiment": {
          "running": false,
          "kind": "pi",
          "duration_ms": 5000,
          "on_ms": 200,
          "period_ms": 1000,
          "core": 0,
          "priority": 2,
          "elapsed_ms": 0,
          "progress_pct": 0,
          "pi": 0,
          "iterations": 0,
          "active": false,
          "sample_ms": 100,
          "enabled": false
        },
        "led": {
          "supported": false,
          "enabled": false,
          "period_ms": 1000,
          "duty_pct": 50,
          "gpio": -1,
          "active_low": true,
          "level": false,
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "available_drivers": [
            "rmt"
          ],
          "driver": "rmt",
          "requested_driver": "rmt",
          "driver_error": "",
          "supported": true,
          "enabled": true,
          "gpio": 8,
          "count": 1,
          "mode": "solid",
          "color": 16752688,
          "brightness_pct": 20,
          "period_ms": 1000,
          "pixels": [
            3350537
          ]
        }
      }
    }
  },
  {
    "tag": "s3",
    "base": "http://192.0.2.2",
    "measured_utc": "2026-10-26T07:02:05.784Z",
    "info": {
      "v": 2,
      "source": "device",
      "boot_id": "s3-500931da-27af2cac",
      "chip": {
        "model": "ESP32-S3",
        "revision_major": 0,
        "revision_minor": 2,
        "cores": 2,
        "psram": true,
        "features": 18
      },
      "firmware": {
        "name": "NaWServer_touch_lcd7",
        "version": "0.1.0-touch-lcd7",
        "idf_version": "5.5.6",
        "http_backend": "idf_httpd",
        "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
        "build_date": "Oct 26 2026",
        "build_time": "10:59:26",
        "reset_reason": 1
      },
      "capabilities": {
        "tasks": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "memory": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "experiments": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "led": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "connections": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "session_details_not_exposed"
        },
        "simulator_control": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "device"
        },
        "sse": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "authenticated_ws": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "pre_handshake_auth_unavailable"
        }
      },
      "limits": {
        "frame_bytes": 4096,
        "body_bytes": 4096,
        "task_page_max": 8,
        "max_ws_clients": 4
      },
      "details": {
        "hardware": {
          "chip_model": "ESP32-S3",
          "architecture": "Xtensa LX7",
          "cpu_mhz": 240,
          "crystal_mhz": 40,
          "apb_mhz": 80,
          "revision_major": 0,
          "revision_minor": 2,
          "cores": 2,
          "wifi_24ghz": true,
          "wifi_6": false,
          "bluetooth_le": true,
          "bluetooth_classic": false,
          "ieee802154": false,
          "usb_serial_jtag": true,
          "hp_sram_bytes": 524288,
          "lp_sram_bytes": 16384
        },
        "flash_memory": {
          "flash_bytes": 16777216,
          "flash_jedec_id": "0xC84018",
          "embedded_flash": false,
          "heap_internal_bytes": 283955,
          "heap_dma_bytes": 275787,
          "psram_bytes": 7120100,
          "app_partition": "factory",
          "app_partition_bytes": 5242880
        },
        "mac_addresses": {
          "factory_mac": "DEVICE-MAC",
          "custom_mac": null,
          "base_mac": "DEVICE-MAC",
          "wifi_sta_mac": "DEVICE-MAC",
          "wifi_ap_mac": "DEVICE-MAC",
          "bluetooth_mac": "DEVICE-MAC",
          "ethernet_mac": "DEVICE-MAC",
          "ieee802154_eui64": null
        },
        "network": {
          "wifi_connected": true,
          "wifi_mode": "STA",
          "ipv4": "192.0.2.2",
          "netmask": "255.255.240.0",
          "gateway": "192.0.2.4",
          "ssid": "<SSID>",
          "ap_bssid": "DEVICE-MAC",
          "channel": 6,
          "rssi_dbm": -73,
          "wifi_auth_mode": 7
        },
        "board": {
          "lcd_interface": "RGB565",
          "lcd_resolution": "800 x 480",
          "lcd_ready": true,
          "backlight_enabled": true,
          "touch": true,
          "touch_controller": "GT911",
          "renderer": "LVGL 8.3.11",
          "framebuffers": 2,
          "rgb_count": 10,
          "rgb_gpio": 6,
          "rgb_order": "GRB",
          "rgb_driver": "RMT",
          "rgb_ready": true,
          "ble_started": false
        },
        "build": {
          "project": "NaWServer_touch_lcd7",
          "version": "0.1.0-touch-lcd7",
          "idf_version": "5.5.6",
          "build_date": "Oct 26 2026",
          "build_time": "10:59:26",
          "secure_version": 0,
          "elf_sha256": "1722470e44683f7e9be2da7f7b4803f78ce23e0292f106c3d89589d944a8775f",
          "reset_reason": 1
        }
      }
    },
    "summary": {
      "v": 2,
      "source": "device",
      "boot_id": "s3-500931da-27af2cac",
      "seq": 3374,
      "captured_ms": 337365,
      "data": {
        "uptime_ms": 337365,
        "sample_interval_ms": 1000,
        "cpu_busy_pct": 3.1789046328610127,
        "core_busy_pct": [
          6.354809355719326,
          0.0029999100026989467
        ],
        "memory": [
          {
            "id": "internal",
            "label": "Внутренняя RAM",
            "total_bytes": 283955,
            "free_bytes": 72519,
            "min_free_bytes": 18872,
            "largest_block_bytes": 31744
          },
          {
            "id": "psram",
            "label": "PSRAM",
            "total_bytes": 7120100,
            "free_bytes": 5579988,
            "min_free_bytes": 5579816,
            "largest_block_bytes": 5505024
          }
        ],
        "rssi_dbm": -73,
        "task_count": 16,
        "ws_clients": 0,
        "network_status": "connected",
        "counters": {
          "http_requests": 27,
          "ws_rx_frames": 200,
          "ws_tx_frames": 382,
          "ws_rx_bytes": 16800,
          "ws_tx_bytes": 271184,
          "rejected": 30
        },
        "experiment": {
          "running": false,
          "kind": "pi",
          "duration_ms": 5000,
          "on_ms": 200,
          "period_ms": 1000,
          "core": 0,
          "priority": 2,
          "elapsed_ms": 0,
          "progress_pct": 0,
          "pi": 0,
          "iterations": 0,
          "active": false,
          "sample_ms": 100,
          "enabled": false
        },
        "led": {
          "supported": false,
          "enabled": false,
          "period_ms": 1000,
          "duty_pct": 50,
          "gpio": -1,
          "active_low": true,
          "level": false,
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "driver": "rmt",
          "requested_driver": "rmt",
          "driver_error": "",
          "supported": true,
          "enabled": false,
          "gpio": 6,
          "count": 10,
          "mode": "solid",
          "color": 16752688,
          "brightness_pct": 20,
          "period_ms": 1000,
          "pixels": [
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0
          ]
        },
        "display": {
          "ready": true,
          "width": 800,
          "height": 480,
          "pattern": 0,
          "backlight": true,
          "pressed": false,
          "x": 0,
          "y": 0,
          "touches": 0,
          "touch_errors": 0,
          "renderer": "lvgl-8.3.11",
          "framebuffers": 2,
          "frame_swaps": 2,
          "frame_events": 9860,
          "vsyncs": 9860
        },
        "network": {
          "socket_limit": 24,
          "tcp_pcb_limit": 32,
          "reserved_sockets": 3,
          "httpd_limit": 21,
          "httpd_open": 2,
          "httpd_free": 19,
          "http_reserve": 3,
          "ws_limit": 4,
          "ws_clients": 0,
          "sse_limit": 4,
          "sse_clients": 0,
          "admission_rejected": 0,
          "last_rejection": "none",
          "retry_after_ms": 2000,
          "captured_ms": 337421,
          "ready": true
        }
      }
    }
  }
]
```

## 2026-10-26 12:03 +05:00 — итог восстановления и аппаратной проверки

S3 восстановлена через CH343 COM11. На ней записан окончательный образ `local/wsfix-s3-final/NaWServer_touch_lcd7.bin`, 1391888 байт, native HTTPD / SDK5.5.6, штатный стек6144, обычный USB selector. EXIO5/USB diagnostic и пробное увеличение стека убраны. board.cpp совпадает с исходным; текущая S3 main.cpp добавляет reason/retries и отключает modem power-save через WIFI_PS_NONE, как существующая C6 demo. Причину улучшения первоначального обнаружения AP одной этой настройкой строго не изолировали; доказаны успешное подключение и работа окончательного образа. Лимит повторов не менялся.

Первичный дефект 101-without-session устранён включением обязательного post-handshake callback в native конфигурациях IDF5.5.6 и capability-based CMake guard; endpoint/protocol не переписывался. В INFO обеих плат firmware.http_backend=idf_httpd; интерфейс показывает INFO → «Сборка» → «HTTP / WS / SSE» = ESP-IDF HTTPD.

Финальные проверки:

- C6: прежний исправленный образ, 27 checks повторно; минутный WS PASS 59/59 echo +60 summary. Финальный HTTP INFO/summary снова успешен, тот же boot, uptime около 59 минут (это не формальный часовой soak-test).
- S3 окончательный образ: PASS 27 checks (HTTP, INFO, CBOR, UTF-8/binary, 20 reconnect, SSE и WS/HTTP при открытом SSE). 180 секунд непрерывного WS: 178/178 точных echo и180 summary, PASS; HTTP API и локальные JS/CSS assets HTTP200 во время обмена. После этого независимый raw Go RFC6455 + CBOR echo также PASS с новым capture tag s3final. INFO ELF SHA256 совпадает с текущим локальным ELF.
- UART capture окончательного образа за215 секунд: не обнаружены Wi-Fi disconnect, reconnect-limit, panic, stack-canary или watchdog. Есть сообщения unsupported/eFuse custom MAC при запросе INFO и recv104 при закрытии тестовых соединений; не объявляются отсутствующими или устранёнными. Минимум стека httpd228 байт отмечен как ограничение, длительная нагрузочная приёмка не утверждается.
- Финальные снимки обоих физических устройств и Flash/RAM приведены выше. S3 internal free72519, min18872, largest31744; PSRAM free5579988 байт. C6 internal free229984, min207516, largest208896 байт. Снимки не являются доказательством отсутствия утечек.
- Host regression ранее в этой правке: Windows CTest46/46; frontend12/12; обе Vue/TypeScript сборки; CMake negative/positive SDK5 backport cases; реальные SDK5.5.6 C6/S3 и SDK6.1 C3 compile-check сборки. git diff --check PASS. Uncrustify отсутствует в текущем PATH/описанном local tools path, не устанавливался; автоматическая проверка форматирования в этой завершающей части не выполнена.

Оба адреса отвечали при финальном снимке: http://192.0.2.1/ (C6), http://192.0.2.2/ (S3), source=device. S3 UART COM11 освобождён после capture. Никаких обновлений SDK/драйверов/пакетов, commit или push в этой проверке не выполнялось. Соседние проекты не менялись.

### Runtime C6 — 2026-10-26T07:29:07.659Z (UTC)

Подключённая плата, http://192.0.2.1. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "c6",
  "base": "http://192.0.2.1",
  "measured_utc": "2026-10-26T07:29:07.659Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 30784,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339452,
          "free_bytes": 205988,
          "min_free_bytes": 199308,
          "largest_block_bytes": 188416
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 35790,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339452,
          "free_bytes": 202468,
          "min_free_bytes": 189836,
          "largest_block_bytes": 176128
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-C6",
    "revision_major": 0,
    "revision_minor": 1,
    "cores": 1,
    "psram": false,
    "features": 82
  },
  "firmware": {
    "name": "NaWServer_c6_lcd147",
    "version": "0.1.0-c6-demo",
    "idf_version": "5.5.6",
    "http_backend": "tcp",
    "board_label": "Waveshare C6 LCD 1.47",
    "build_date": "Oct 26 2026",
    "build_time": "12:26:04",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "c6-cd721cbb-8664a17d",
    "seq": 351,
    "captured_ms": 35485,
    "cpu_busy_pct": 40.3,
    "free_bytes": 202672,
    "task_count": 12
  },
  "status": "PASS"
}
```

### Runtime S3 — 2026-10-26T07:31:57.259Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T07:31:57.259Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```
<!-- backend26-19-01FCF21B6E07 -->
### 2026-10-26 12:32:04 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 19

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/19`; SDKCONFIG SHA256-prefix `01FCF21B6E07`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1391888 байт**; linker total_size: **1391777 байт**; статические data+bss: **74728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 893386 | 0 | 0 |
| Flash Data | 350972 | 0 | 0 |
| DIRAM | 178243 | 341760 | 163517 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-20-4A1CA130F076 -->
### 2026-10-26 12:32:04 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 20

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/20`; SDKCONFIG SHA256-prefix `4A1CA130F076`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1188448 байт**; linker total_size: **1188086 байт**; статические data+bss: **72456 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1040094 | 0 | 0 |
| DIRAM | 198628 | 452112 | 253484 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-25-A41C8FCC0E15 -->
### 2026-10-26 12:32:04 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 25

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/25`; SDKCONFIG SHA256-prefix `A41C8FCC0E15`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1371936 байт**; linker total_size: **1371821 байт**; статические data+bss: **74752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 883710 | 0 | 0 |
| Flash Data | 340828 | 0 | 0 |
| DIRAM | 178123 | 341760 | 163637 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-26-8292D260BC34 -->
### 2026-10-26 12:32:05 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 26

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/26`; SDKCONFIG SHA256-prefix `8292D260BC34`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1166016 байт**; linker total_size: **1165648 байт**; статические data+bss: **72488 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1017870 | 0 | 0 |
| DIRAM | 198438 | 452112 | 253674 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### Runtime S3 — 2026-10-26T07:32:53.376Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T07:32:53.376Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

### Runtime S3 — 2026-10-26T07:33:48.265Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T07:33:48.265Z",
  "checks": 0,
  "snapshots": [],
  "status": "FAIL",
  "error": "TimeoutError: The operation was aborted due to timeout"
}
```

## 2026-10-26 — TCP варианты C6/S3, дневные прогоны до двух минут

Текущее поручение владельца: подготовить TCP, днём не запускать нагрузочные прогоны дольше двух минут; длинная нагрузка отложена на ночь. Ночной job не создавался, время не назначалось.

В демо добавлены отдельные PlatformIO профили c6_tcp / s3_tcp со своим sdkconfig.tcp.defaults и отдельным .pio/sdkconfig state, SDK5.5.6 сохранён. Компонент TCP уже был реализован; примеры переведены на существующие HttpServer, tcp::IdfTransport, TcpHttpBackend, sse::TcpAdapter. Native HTTPD профили сохранены и повторно собраны. C6 использует общий WebSocketExchange для закрытия callback. S3 native SDK session enumeration остаётся только в native ветке; TCP получает число owned handles транспорта минус listener. Поля network httpd_* сохранены как legacy CBOR/UI contract для вместимости выбранного backend; firmware.http_backend показывает фактический backend.

Первый C6 TCP образ прошит после свежего определения MAC DEVICE-MAC/COM13, штатная проверка записи прошла, функциональный прогон PASS27 с INFO backend=tcp. Первый S3 TCP образ прошит по COM11 после свежего определения MAC DEVICE-MAC. Оба ELF не содержат символов httpd_start/httpd_ws_send_frame. Начальные размеры/конфиги профилей25/26 записаны выше, включая ранние ошибки.

S3 первый TCP образ не прошёл INFO: UART local/tcp27-s3-info-uart.log зафиксировал `***ERROR*** A stack overflow in task naw_tcp_http has been detected`, backtrace CORRUPTED, reboot RTC_SW_CPU_RST. Главная страница и summary отвечали до INFO, поэтому initial HTTP timeouts нельзя считать только потерей Wi-Fi. Владелец также наблюдал перезагрузку при подключении к S3. Это доказанный дефект бюджета стека TCP worker6144 в этом демо.

Добавлена ESP-IDF-only Kconfig настройка NAWSERVER_TCP_HTTP_STACK_SIZE (default6144, range4096..65536); S3 TCP профиль задаёт8192, C6 оставляет6144. SSE worker4096, host threads и native HTTPD не меняются. Исправленный образ пока собирается, результаты после его прошивки должны быть добавлены ниже. Поведение при перегрузе WS в TCP использует bounded admission общего engine и transport close; native ручной close1013 сохранён в native ветке. Полная эквивалентность pressure/reconnect/long-soak не утверждается.

### Runtime S3 — 2026-10-26T07:41:15.335Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T07:41:15.335Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 25264,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284187,
          "free_bytes": 37719,
          "min_free_bytes": 8188,
          "largest_block_bytes": 27648
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7130252,
          "free_bytes": 5590140,
          "min_free_bytes": 5590140,
          "largest_block_bytes": 5505024
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 30264,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284187,
          "free_bytes": 32387,
          "min_free_bytes": 7796,
          "largest_block_bytes": 16384
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7130252,
          "free_bytes": 5590140,
          "min_free_bytes": 5590140,
          "largest_block_bytes": 5505024
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "http_backend": "tcp",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "12:37:04",
    "reset_reason": 1
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-5b38b7ef-86170c6d",
    "seq": 301,
    "captured_ms": 30065,
    "cpu_busy_pct": 4.5,
    "free_bytes": 35495,
    "task_count": 16
  },
  "status": "PASS"
}
```

### Runtime C6 — 2026-10-26T07:43:16.004Z (UTC)

Подключённая плата, http://192.0.2.1. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "c6",
  "base": "http://192.0.2.1",
  "measured_utc": "2026-10-26T07:43:16.004Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 151683,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339452,
          "free_bytes": 198868,
          "min_free_bytes": 192292,
          "largest_block_bytes": 180224
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 156783,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 339452,
          "free_bytes": 196300,
          "min_free_bytes": 180084,
          "largest_block_bytes": 180224
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-C6",
    "revision_major": 0,
    "revision_minor": 1,
    "cores": 1,
    "psram": false,
    "features": 82
  },
  "firmware": {
    "name": "NaWServer_c6_lcd147",
    "version": "0.1.0-c6-demo",
    "idf_version": "5.5.6",
    "http_backend": "tcp",
    "board_label": "Waveshare C6 LCD 1.47",
    "build_date": "Oct 26 2026",
    "build_time": "12:37:04",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "c6-41456555-8379f291",
    "seq": 1561,
    "captured_ms": 156484,
    "cpu_busy_pct": 39,
    "free_bytes": 196400,
    "task_count": 12
  },
  "status": "PASS"
}
```

### WebSocket: непрерывное соединение (длительность и платы в JSON)

```json
[
  {
    "tag": "c6",
    "base": "http://192.0.2.1",
    "started_utc": "2026-10-26T07:43:06.099Z",
    "duration_seconds": 60,
    "echo_sent": 59,
    "echo_received": 59,
    "summaries": 60,
    "backend": "tcp",
    "boot_id": "c6-41456555-8379f291",
    "status": "PASS",
    "finished_utc": "2026-10-26T07:44:06.314Z"
  },
  {
    "tag": "s3",
    "base": "http://192.0.2.2",
    "started_utc": "2026-10-26T07:43:06.115Z",
    "duration_seconds": 60,
    "echo_sent": 59,
    "echo_received": 59,
    "summaries": 132,
    "backend": "tcp",
    "boot_id": "s3-5b38b7ef-86170c6d",
    "status": "PASS",
    "finished_utc": "2026-10-26T07:44:06.325Z"
  }
]
```
<!-- backend26-25-1BE644AB1997 -->
### 2026-10-26 12:48:05 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 25

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/25`; SDKCONFIG SHA256-prefix `1BE644AB1997`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1371952 байт**; linker total_size: **1371829 байт**; статические data+bss: **74752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 883718 | 0 | 0 |
| Flash Data | 340828 | 0 | 0 |
| DIRAM | 178123 | 341760 | 163637 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-26-F8026ADE0799 -->
### 2026-10-26 12:48:05 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 26

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/26`; SDKCONFIG SHA256-prefix `F8026ADE0799`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **1166016 байт**; linker total_size: **1165648 байт**; статические data+bss: **72488 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1017870 | 0 | 0 |
| DIRAM | 198438 | 452112 | 253674 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-3-BA7A70E69832 -->
### 2026-10-26 12:48:06 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 3

Исходники: `632dafd` + переход конфигураций SDK и исправление WS post-handshake; build `local/backend26-idf/3`; SDKCONFIG SHA256-prefix `BA7A70E69832`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
```

BIN: **958576 байт**; linker total_size: **958198 байт**; статические data+bss: **50248 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 728028 | 0 | 0 |
| Flash Data | 144228 | 0 | 0 |
| DRAM | 123034 | 321296 | 198262 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### Runtime S3 — 2026-10-26T07:49:57.006Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T07:49:57.006Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 95364,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284187,
          "free_bytes": 41707,
          "min_free_bytes": 7884,
          "largest_block_bytes": 30720
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7130252,
          "free_bytes": 5590140,
          "min_free_bytes": 5590140,
          "largest_block_bytes": 5505024
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 101564,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284187,
          "free_bytes": 41435,
          "min_free_bytes": 7796,
          "largest_block_bytes": 30720
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7130252,
          "free_bytes": 5590140,
          "min_free_bytes": 5590140,
          "largest_block_bytes": 5505024
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "http_backend": "tcp",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "12:37:04",
    "reset_reason": 1
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-359df896-e0cf70fa",
    "seq": 1011,
    "captured_ms": 101065,
    "cpu_busy_pct": 10.1,
    "free_bytes": 41311,
    "task_count": 16
  },
  "status": "PASS"
}
```

### WebSocket: непрерывное соединение (длительность и платы в JSON)

```json
[
  {
    "tag": "s3",
    "base": "http://192.0.2.2",
    "started_utc": "2026-10-26T07:50:12.173Z",
    "duration_seconds": 60,
    "echo_sent": 59,
    "echo_received": 59,
    "summaries": 60,
    "backend": "tcp",
    "boot_id": "s3-359df896-e0cf70fa",
    "status": "PASS",
    "finished_utc": "2026-10-26T07:51:12.416Z"
  }
]
```

### S3 TCP: повторный INFO, лимит заголовка и память

```json
{
  "measured_utc": "2026-10-26T07:51:38.636Z",
  "checks": [
    {
      "name": "INFO",
      "status": 200,
      "boot": "s3-359df896-e0cf70fa"
    },
    {
      "name": "INFO",
      "status": 200,
      "boot": "s3-359df896-e0cf70fa"
    },
    {
      "name": "INFO",
      "status": 200,
      "boot": "s3-359df896-e0cf70fa"
    },
    {
      "name": "INFO",
      "status": 200,
      "boot": "s3-359df896-e0cf70fa"
    },
    {
      "name": "INFO",
      "status": 200,
      "boot": "s3-359df896-e0cf70fa"
    },
    {
      "name": "INFO",
      "status": 200,
      "boot": "s3-359df896-e0cf70fa"
    },
    {
      "name": "header_limit",
      "status": 431
    }
  ],
  "summary": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-359df896-e0cf70fa",
    "seq": 1974,
    "captured_ms": 197364,
    "data": {
      "uptime_ms": 197364,
      "sample_interval_ms": 1000,
      "cpu_busy_pct": 20.3030333426955,
      "core_busy_pct": [
        28.89700276852706,
        11.709063916863938
      ],
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284187,
          "free_bytes": 35059,
          "min_free_bytes": 4204,
          "largest_block_bytes": 15360
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7130252,
          "free_bytes": 5590140,
          "min_free_bytes": 5590140,
          "largest_block_bytes": 5505024
        }
      ],
      "rssi_dbm": -66,
      "task_count": 16,
      "ws_clients": 2,
      "network_status": "connected",
      "counters": {
        "http_requests": 475,
        "ws_rx_frames": 80,
        "ws_tx_frames": 386,
        "ws_rx_bytes": 6572,
        "ws_tx_bytes": 433707,
        "rejected": 9
      },
      "experiment": {
        "running": false,
        "kind": "pi",
        "duration_ms": 5000,
        "on_ms": 200,
        "period_ms": 1000,
        "core": 0,
        "priority": 2,
        "elapsed_ms": 0,
        "progress_pct": 0,
        "pi": 0,
        "iterations": 0,
        "active": false,
        "sample_ms": 100,
        "enabled": false
      },
      "led": {
        "supported": false,
        "enabled": false,
        "period_ms": 1000,
        "duty_pct": 50,
        "gpio": -1,
        "active_low": true,
        "level": false,
        "reason": "board_led_not_confirmed"
      },
      "strip": {
        "driver": "rmt",
        "requested_driver": "rmt",
        "driver_error": "",
        "supported": true,
        "enabled": false,
        "gpio": 6,
        "count": 10,
        "mode": "solid",
        "color": 16752688,
        "brightness_pct": 20,
        "period_ms": 1000,
        "pixels": [
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0
        ]
      },
      "display": {
        "ready": true,
        "width": 800,
        "height": 480,
        "pattern": 0,
        "backlight": true,
        "pressed": false,
        "x": 0,
        "y": 0,
        "touches": 0,
        "touch_errors": 0,
        "renderer": "lvgl-8.3.11",
        "framebuffers": 2,
        "frame_swaps": 2,
        "frame_events": 5763,
        "vsyncs": 5763
      },
      "network": {
        "socket_limit": 24,
        "tcp_pcb_limit": 32,
        "reserved_sockets": 3,
        "httpd_limit": 21,
        "httpd_open": 5,
        "httpd_free": 16,
        "http_reserve": 3,
        "ws_limit": 4,
        "ws_clients": 2,
        "sse_limit": 4,
        "sse_clients": 2,
        "admission_rejected": 0,
        "last_rejection": "none",
        "retry_after_ms": 2000,
        "captured_ms": 197428,
        "ready": true
      }
    }
  },
  "workers": [
    {
      "id": "task-15",
      "name": "naw_tcp_http",
      "state": "blocked",
      "priority": 5,
      "core_affinity": null,
      "stack_min_free_bytes": 1956,
      "runtime_ticks": 3950778,
      "cpu_pct": 2.160757735314678
    },
    {
      "id": "task-14",
      "name": "naw_tcp_sse",
      "state": "blocked",
      "priority": 4,
      "core_affinity": null,
      "stack_min_free_bytes": 2024,
      "runtime_ticks": 9865500,
      "cpu_pct": 2.7251989395225853
    }
  ]
}
```

## 2026-10-26 — автономный MCP, финальная интеграция TCP и демо

Основание: поручение владельца реализовать MCP параллельными подагентами и
встроить в красивое и тестовое демо. Исходники: 632dafd + текущие изменения,
без коммита. Среда и пакеты не обновлялись. Ниже новые измерения именно MCP-on;
предыдущие TCP/native измерения выше остаются историческими.

Реализация: optional NaWServer_MCP=1 + NaWServer_JSON_BOUNDED=1, прямой POST /mcp,
современная ветка протокола 2026-07-28 (server/discover, tools/list, tools/call),
явная legacy-ветка initialize 2025-11-25. Только device_info и echo, без изменения
устройства. JSON-only stateless subset; произвольная совместимость всех MCP
клиентов, ресурсов/prompts/tasks/SSE-сессий не заявлена. Workspace 128 tokens
по8 B +2048 B output =3072 B; C6/C3 static, S3 PSRAM без internal fallback.
Превышение output capacity возвращает JSON-RPC -32603/HTTP500, не обрезанный успех.

S3 TCP: сохранён исправленный HTTP task stack8192 B и header workspace2048 B;
C6 task stack6144 B. Предыдущий S3 crash был stack overflow naw_tcp_http, а
не завершённый успешный WS-сеанс. Общая библиотека MCP-off CTest46/46 прошла;
MCP/JSON CTest2/2, корпус135 проверок; три неверные конфигурации отвергаются.

Живые локальные сайты: C++ TCP test http://127.0.0.1:8083/ и C# модель
http://127.0.0.1:8798/#lab. Повторный modern MCP smoke passed на обоих; CUA
подтвердил discovery/list/UTF-8 echo и18/18 HTTP/WS/SSE на8083. 8798 явно
помечен «Модель симулятора». Это отдельная C# реализация, не embedded runtime.
Попытка заменить старый процесс8788 автоматическим перезапуском отклонена
автоматической проверкой безопасности без указанной причины; процесс105324
не изменён. Обход блокировки не выполнялся; новая версия доступна на8798.

Длительные нагрузки не запускались; дневные сеансы ограничены двумя минутами.

### Runtime S3 — 2026-10-26T08:44:55.338Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T08:44:55.338Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 27464,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284131,
          "free_bytes": 41863,
          "min_free_bytes": 7956,
          "largest_block_bytes": 31744
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7112948,
          "free_bytes": 5569760,
          "min_free_bytes": 5569760,
          "largest_block_bytes": 5505024
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 32364,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284131,
          "free_bytes": 41811,
          "min_free_bytes": 7796,
          "largest_block_bytes": 21504
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7112948,
          "free_bytes": 5569760,
          "min_free_bytes": 5569760,
          "largest_block_bytes": 5505024
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "http_backend": "tcp",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "13:16:05",
    "reset_reason": 1
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-401f02a3-4e18a817",
    "seq": 321,
    "captured_ms": 32065,
    "cpu_busy_pct": 4.8,
    "free_bytes": 40007,
    "task_count": 16
  },
  "status": "PASS"
}
```

### Runtime C6 — 2026-10-26T08:44:55.246Z (UTC)

Подключённая плата, http://192.0.2.1. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "c6",
  "base": "http://192.0.2.1",
  "measured_utc": "2026-10-26T08:44:55.246Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 15384,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 336316,
          "free_bytes": 200000,
          "min_free_bytes": 186016,
          "largest_block_bytes": 184320
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 20884,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 336316,
          "free_bytes": 199576,
          "min_free_bytes": 177508,
          "largest_block_bytes": 184320
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-C6",
    "revision_major": 0,
    "revision_minor": 1,
    "cores": 1,
    "psram": false,
    "features": 82
  },
  "firmware": {
    "name": "NaWServer_c6_lcd147",
    "version": "0.1.0-c6-demo",
    "idf_version": "5.5.6",
    "http_backend": "tcp",
    "board_label": "Waveshare C6 LCD 1.47",
    "build_date": "Oct 26 2026",
    "build_time": "13:16:07",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "c6-1f0856e5-df3a70f8",
    "seq": 201,
    "captured_ms": 20485,
    "cpu_busy_pct": 25.4,
    "free_bytes": 196248,
    "task_count": 12
  },
  "status": "PASS"
}
```

### Финальные MCP-on runtime и проверка соответствия ELF

```json
[
  {
    "board": "c6",
    "base": "http://192.0.2.1",
    "profile": 30,
    "measured_utc": "2026-10-26T08:45:56.083Z",
    "elf_sha256": "cbc50b073370675f273843772d2f641a58e8ac8a700b099c6251b1de1400e2c1",
    "info": {
      "v": 2,
      "source": "device",
      "boot_id": "c6-1f0856e5-df3a70f8",
      "chip": {
        "model": "ESP32-C6",
        "revision_major": 0,
        "revision_minor": 1,
        "cores": 1,
        "psram": false,
        "features": 82
      },
      "firmware": {
        "name": "NaWServer_c6_lcd147",
        "version": "0.1.0-c6-demo",
        "idf_version": "5.5.6",
        "http_backend": "tcp",
        "board_label": "Waveshare C6 LCD 1.47",
        "build_date": "Oct 26 2026",
        "build_time": "13:16:07",
        "reset_reason": 11
      },
      "capabilities": {
        "tasks": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "memory": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "experiments": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "led": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "connections": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "session_details_not_exposed"
        },
        "simulator_control": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "device"
        },
        "sse": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "authenticated_ws": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "pre_handshake_auth_unavailable"
        }
      },
      "details": {
        "hardware": {
          "chip_model": "ESP32-C6",
          "architecture": "RISC-V",
          "cpu_mhz": 160,
          "crystal_mhz": 40,
          "apb_mhz": 40,
          "revision_major": 0,
          "revision_minor": 1,
          "cores": 1,
          "wifi_24ghz": true,
          "wifi_6": true,
          "bluetooth_le": true,
          "bluetooth_classic": false,
          "ieee802154": true,
          "usb_serial_jtag": true,
          "hp_sram_bytes": 524288,
          "lp_sram_bytes": 16384
        },
        "flash_memory": {
          "flash_bytes": 4194304,
          "flash_jedec_id": "0x204016",
          "embedded_flash": true,
          "heap_internal_bytes": 336316,
          "heap_dma_bytes": 319988,
          "psram_bytes": 0,
          "app_partition": "factory",
          "app_partition_bytes": 3145728
        },
        "mac_addresses": {
          "factory_mac": "DEVICE-MAC",
          "custom_mac": null,
          "base_mac": "DEVICE-MAC",
          "wifi_sta_mac": "DEVICE-MAC",
          "wifi_ap_mac": "DEVICE-MAC",
          "bluetooth_mac": "DEVICE-MAC",
          "ethernet_mac": "DEVICE-MAC",
          "ieee802154_eui64": "DEVICE-MAC:1F:5C"
        },
        "network": {
          "wifi_connected": true,
          "wifi_mode": "STA",
          "ipv4": "192.0.2.1",
          "netmask": "255.255.240.0",
          "gateway": "192.0.2.4",
          "ssid": "<SSID>",
          "ap_bssid": "DEVICE-MAC",
          "channel": 6,
          "rssi_dbm": -80,
          "wifi_auth_mode": 7
        },
        "board": {
          "lcd_controller": "ST7789T",
          "lcd_resolution": "172 x 320",
          "lcd_spi_ready": true,
          "lcd_visual_status": "unverified",
          "backlight_percent": 30,
          "rgb_count": 1,
          "rgb_gpio": 8,
          "rgb_order": "RGB",
          "rgb_driver": "RMT",
          "sd_slot": true,
          "sd_mounted": false,
          "touch": false,
          "ble_started": false
        },
        "build": {
          "project": "NaWServer_c6_lcd147",
          "version": "0.1.0-c6-demo",
          "idf_version": "5.5.6",
          "build_date": "Oct 26 2026",
          "build_time": "13:16:07",
          "secure_version": 0,
          "elf_sha256": "cbc50b073370675f273843772d2f641a58e8ac8a700b099c6251b1de1400e2c1",
          "reset_reason": 11
        }
      },
      "limits": {
        "frame_bytes": 4096,
        "body_bytes": 4096,
        "task_page_max": 8,
        "max_ws_clients": 4
      }
    },
    "summary": {
      "v": 2,
      "source": "device",
      "boot_id": "c6-1f0856e5-df3a70f8",
      "seq": 756,
      "captured_ms": 75984,
      "data": {
        "uptime_ms": 75984,
        "sample_interval_ms": 1000,
        "cpu_busy_pct": 11.469541218351264,
        "core_busy_pct": [
          11.469541218351264
        ],
        "memory": [
          {
            "id": "internal",
            "label": "Внутренняя RAM",
            "total_bytes": 336316,
            "free_bytes": 180068,
            "min_free_bytes": 152584,
            "largest_block_bytes": 159744
          }
        ],
        "rssi_dbm": -80,
        "task_count": 12,
        "ws_clients": 3,
        "network_status": "connected",
        "counters": {
          "http_requests": 240,
          "ws_rx_frames": 68,
          "ws_tx_frames": 217,
          "ws_rx_bytes": 5564,
          "ws_tx_bytes": 140274,
          "rejected": 5
        },
        "experiment": {
          "running": false,
          "kind": "pi",
          "duration_ms": 5000,
          "on_ms": 200,
          "period_ms": 1000,
          "core": 0,
          "priority": 2,
          "elapsed_ms": 0,
          "progress_pct": 0,
          "pi": 0,
          "iterations": 0,
          "active": false,
          "sample_ms": 100,
          "enabled": false
        },
        "led": {
          "supported": false,
          "enabled": false,
          "period_ms": 1000,
          "duty_pct": 50,
          "gpio": -1,
          "active_low": true,
          "level": false,
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "available_drivers": [
            "rmt"
          ],
          "driver": "rmt",
          "requested_driver": "rmt",
          "driver_error": "",
          "supported": true,
          "enabled": true,
          "gpio": 8,
          "count": 1,
          "mode": "solid",
          "color": 16752688,
          "brightness_pct": 20,
          "period_ms": 1000,
          "pixels": [
            3350537
          ]
        }
      }
    }
  },
  {
    "board": "s3",
    "base": "http://192.0.2.2",
    "profile": 29,
    "measured_utc": "2026-10-26T08:45:56.187Z",
    "elf_sha256": "7584862a5485d945747d91e1eef99947c627480e4109d80eb9ddfc850f5a8a59",
    "info": {
      "v": 2,
      "source": "device",
      "boot_id": "s3-401f02a3-4e18a817",
      "chip": {
        "model": "ESP32-S3",
        "revision_major": 0,
        "revision_minor": 2,
        "cores": 2,
        "psram": true,
        "features": 18
      },
      "firmware": {
        "name": "NaWServer_touch_lcd7",
        "version": "0.1.0-touch-lcd7",
        "idf_version": "5.5.6",
        "http_backend": "tcp",
        "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
        "build_date": "Oct 26 2026",
        "build_time": "13:16:05",
        "reset_reason": 1
      },
      "capabilities": {
        "tasks": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "memory": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "experiments": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "led": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "connections": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "session_details_not_exposed"
        },
        "simulator_control": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "device"
        },
        "sse": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "authenticated_ws": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "pre_handshake_auth_unavailable"
        }
      },
      "limits": {
        "frame_bytes": 4096,
        "body_bytes": 4096,
        "task_page_max": 8,
        "max_ws_clients": 4
      },
      "details": {
        "hardware": {
          "chip_model": "ESP32-S3",
          "architecture": "Xtensa LX7",
          "cpu_mhz": 240,
          "crystal_mhz": 40,
          "apb_mhz": 80,
          "revision_major": 0,
          "revision_minor": 2,
          "cores": 2,
          "wifi_24ghz": true,
          "wifi_6": false,
          "bluetooth_le": true,
          "bluetooth_classic": false,
          "ieee802154": false,
          "usb_serial_jtag": true,
          "hp_sram_bytes": 524288,
          "lp_sram_bytes": 16384
        },
        "flash_memory": {
          "flash_bytes": 16777216,
          "flash_jedec_id": "0xC84018",
          "embedded_flash": false,
          "heap_internal_bytes": 284131,
          "heap_dma_bytes": 275963,
          "psram_bytes": 7112948,
          "app_partition": "factory",
          "app_partition_bytes": 5242880
        },
        "mac_addresses": {
          "factory_mac": "DEVICE-MAC",
          "custom_mac": null,
          "base_mac": "DEVICE-MAC",
          "wifi_sta_mac": "DEVICE-MAC",
          "wifi_ap_mac": "DEVICE-MAC",
          "bluetooth_mac": "DEVICE-MAC",
          "ethernet_mac": "DEVICE-MAC",
          "ieee802154_eui64": null
        },
        "network": {
          "wifi_connected": true,
          "wifi_mode": "STA",
          "ipv4": "192.0.2.2",
          "netmask": "255.255.240.0",
          "gateway": "192.0.2.4",
          "ssid": "<SSID>",
          "ap_bssid": "DEVICE-MAC",
          "channel": 6,
          "rssi_dbm": -68,
          "wifi_auth_mode": 7
        },
        "board": {
          "lcd_interface": "RGB565",
          "lcd_resolution": "800 x 480",
          "lcd_ready": true,
          "backlight_enabled": true,
          "touch": true,
          "touch_controller": "GT911",
          "renderer": "LVGL 8.3.11",
          "framebuffers": 2,
          "rgb_count": 10,
          "rgb_gpio": 6,
          "rgb_order": "GRB",
          "rgb_driver": "RMT",
          "rgb_ready": true,
          "ble_started": false
        },
        "build": {
          "project": "NaWServer_touch_lcd7",
          "version": "0.1.0-touch-lcd7",
          "idf_version": "5.5.6",
          "build_date": "Oct 26 2026",
          "build_time": "13:16:05",
          "secure_version": 0,
          "elf_sha256": "7584862a5485d945747d91e1eef99947c627480e4109d80eb9ddfc850f5a8a59",
          "reset_reason": 1
        }
      }
    },
    "summary": {
      "v": 2,
      "source": "device",
      "boot_id": "s3-401f02a3-4e18a817",
      "seq": 882,
      "captured_ms": 88164,
      "data": {
        "uptime_ms": 88164,
        "sample_interval_ms": 1000,
        "cpu_busy_pct": 11.777735554711095,
        "core_busy_pct": [
          15.73031460629213,
          7.82515650313006
        ],
        "memory": [
          {
            "id": "internal",
            "label": "Внутренняя RAM",
            "total_bytes": 284131,
            "free_bytes": 29119,
            "min_free_bytes": 6000,
            "largest_block_bytes": 13312
          },
          {
            "id": "psram",
            "label": "PSRAM",
            "total_bytes": 7112948,
            "free_bytes": 5569760,
            "min_free_bytes": 5569760,
            "largest_block_bytes": 5505024
          }
        ],
        "rssi_dbm": -66,
        "task_count": 16,
        "ws_clients": 3,
        "network_status": "connected",
        "counters": {
          "http_requests": 208,
          "ws_rx_frames": 68,
          "ws_tx_frames": 210,
          "ws_rx_bytes": 5564,
          "ws_tx_bytes": 203402,
          "rejected": 7
        },
        "experiment": {
          "running": false,
          "kind": "pi",
          "duration_ms": 5000,
          "on_ms": 200,
          "period_ms": 1000,
          "core": 0,
          "priority": 2,
          "elapsed_ms": 0,
          "progress_pct": 0,
          "pi": 0,
          "iterations": 0,
          "active": false,
          "sample_ms": 100,
          "enabled": false
        },
        "led": {
          "supported": false,
          "enabled": false,
          "period_ms": 1000,
          "duty_pct": 50,
          "gpio": -1,
          "active_low": true,
          "level": false,
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "driver": "rmt",
          "requested_driver": "rmt",
          "driver_error": "",
          "supported": true,
          "enabled": false,
          "gpio": 6,
          "count": 10,
          "mode": "solid",
          "color": 16752688,
          "brightness_pct": 20,
          "period_ms": 1000,
          "pixels": [
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0
          ]
        },
        "display": {
          "ready": true,
          "width": 800,
          "height": 480,
          "pattern": 0,
          "backlight": true,
          "pressed": false,
          "x": 0,
          "y": 0,
          "touches": 0,
          "touch_errors": 0,
          "renderer": "lvgl-8.3.11",
          "framebuffers": 2,
          "frame_swaps": 2,
          "frame_events": 2568,
          "vsyncs": 2568
        },
        "network": {
          "socket_limit": 24,
          "tcp_pcb_limit": 32,
          "reserved_sockets": 3,
          "httpd_limit": 21,
          "httpd_open": 6,
          "httpd_free": 15,
          "http_reserve": 3,
          "ws_limit": 4,
          "ws_clients": 3,
          "sse_limit": 4,
          "sse_clients": 2,
          "admission_rejected": 0,
          "last_rejection": "none",
          "retry_after_ms": 2000,
          "captured_ms": 88246,
          "ready": true
        }
      }
    }
  }
]
```
<!-- backend26-27-50E66153B016 -->
### 2026-10-26 13:45:56 +05:00 — NaWServer_showcase, esp32c3, SDK 5.5.6, профиль 27

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/27`; SDKCONFIG SHA256-prefix `50E66153B016`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1134160 байт**; linker total_size: **1133794 байт**; статические data+bss: **66700 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 763638 | 0 | 0 |
| Flash Data | 253620 | 0 | 0 |
| DRAM | 164860 | 321296 | 156436 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-28-CBB7A2686929 -->
### 2026-10-26 13:45:57 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 28

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/28`; SDKCONFIG SHA256-prefix `CBB7A2686929`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1147376 байт**; linker total_size: **1146998 байт**; статические data+bss: **68088 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 782262 | 0 | 0 |
| Flash Data | 260912 | 0 | 0 |
| DRAM | 153276 | 321296 | 168020 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-29-971EAB16DACF -->
### 2026-10-26 13:45:58 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 29

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/29`; SDKCONFIG SHA256-prefix `971EAB16DACF`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1412448 байт**; linker total_size: **1412329 байт**; статические data+bss: **74808 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 906894 | 0 | 0 |
| Flash Data | 358132 | 0 | 0 |
| DIRAM | 178199 | 341760 | 163561 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-30-98C1E4C2863E -->
### 2026-10-26 13:45:58 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 30

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/30`; SDKCONFIG SHA256-prefix `98C1E4C2863E`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1208080 байт**; linker total_size: **1207726 байт**; статические data+bss: **75632 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1058916 | 0 | 0 |
| DIRAM | 201582 | 452112 | 250530 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-31-A86A891A3A94 -->
### 2026-10-26 13:45:58 +05:00 — NaWServer_minimal, esp32c3, SDK 5.5.6, профиль 31

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/31`; SDKCONFIG SHA256-prefix `A86A891A3A94`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **991152 байт**; linker total_size: **990790 байт**; статические data+bss: **51588 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 733616 | 0 | 0 |
| Flash Data | 160436 | 0 | 0 |
| DRAM | 134946 | 321296 | 186350 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### WebSocket: непрерывное соединение (длительность и платы в JSON)

```json
[
  {
    "tag": "c6",
    "base": "http://192.0.2.1",
    "started_utc": "2026-10-26T08:45:08.251Z",
    "duration_seconds": 60,
    "echo_sent": 59,
    "echo_received": 59,
    "summaries": 59,
    "backend": "tcp",
    "boot_id": "c6-1f0856e5-df3a70f8",
    "status": "PASS",
    "finished_utc": "2026-10-26T08:46:08.608Z"
  },
  {
    "tag": "s3",
    "base": "http://192.0.2.2",
    "started_utc": "2026-10-26T08:45:08.273Z",
    "duration_seconds": 60,
    "echo_sent": 59,
    "echo_received": 59,
    "summaries": 60,
    "backend": "tcp",
    "boot_id": "s3-401f02a3-4e18a817",
    "status": "PASS",
    "finished_utc": "2026-10-26T08:46:08.609Z"
  }
]
```
<!-- backend26-32-72EA6F6FFD8E -->
### 2026-10-26 13:47:26 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 32

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/32`; SDKCONFIG SHA256-prefix `72EA6F6FFD8E`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1005648 байт**; linker total_size: **1005272 байт**; статические data+bss: **53456 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 750508 | 0 | 0 |
| Flash Data | 167756 | 0 | 0 |
| DRAM | 126284 | 321296 | 195012 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### Итог MCP-on интеграции

Все шесть новых профилей27–32 собраны: showcase иminimal на местных IDF5.5.6
и6.1.0, реальные C6/S3 на5.5.6; таблица и sdkconfig выше. Для29/30 runtime
измерен в секции «Финальные MCP-on runtime и проверка соответствия ELF» выше:
C6 ELF cbc50b073370675f273843772d2f641a58e8ac8a700b099c6251b1de1400e2c1;
S3 ELF 7584862a5485d945747d91e1eef99947c627480e4109d80eb9ddfc850f5a8a59.
Совпадение с INFO проверено программно. Снимок при3WS-клиентах: C6 free180068,
min152584 B; S3 free29119,min6000 B, PSRAM free5569760 B. Это не idle benchmark
и не чистая разница стоимости MCP: одновременно обновлены frontend assets.

C6 иS3: MCP smoke PASS, прежние27HTTP/WS/SSE checks PASS,60секунд непрерывного
WS59/59echo накаждой, телеметрия59/60 соответственно. GUI discovery/list/
device_info/UTF-8echo PASS, INFO NaWServerTCP/source=device, все3транспортаonline.
S3 SSE сначалаoffline, затем самостоятельноonline безручного retry; случай
не скрыт. Ночной длительный soak иLinux MCP acceptance остаются непроверенными.

Повторная финальная проверка frontend17/17, C#65/65, MCP135checks (CTest2/2),
основная библиотека46/46. git diff --check прошёл после удаления пустыхEOFстрок.
Среда не обновлялась; коммит/пуш не делались. Описание:
docs/readme_mcp_implementation_ru.md. Старый8788процесс не заменён из-за
отклонения автоматической проверки; новаякрасивая версия на8798.

## 2026-10-26 — отдельная страница MCP, запись данных и эксперименты

Поручение владельца: список и выполнение инструментов, получение данных,
управление существующими экспериментами; JSON-библиотека в INFO.
Общий Vue frontend: страница #mcp отдельно от «Лаборатории», таблица tools,
JSON-аргументы, возвращённые данные и раскрываемые raw JSON-RPC / журнал.
На 390×844 панель задач располагается снизу, таблица прокручивается по горизонтали.

В полных демо C6/S3/showcase весь /mcp защищён прежним Auth+Access chain:
readonly discovery тоже требует Basic. Credentials берутся из сеанса раздела
«Доступ» и не записываются в args/transcript. Общий MCP-модуль не владеет паролем.
experiment_status / experiment_set вызывают runtime_state / set_experiment.
Длительность 1000..30000 мс, период 20..5000 мс, активная часть 1..period,
приоритет 1..5; C6/C3 core0, S3 core0..1. Stop сохраняет прежнюю семантику.
MCP не создаёт своего движка нагрузки и не управляет GPIO/LED.

Четыре описания инструментов: 1233 байта / 140 токенов. Workspace теперь
256×8 + 2048 = 4096 байт, на 1024 больше первого релиза. S3 использует PSRAM
без internal fallback, C6/C3 — static. Minimal сохраняет два readonly tools.
INFO firmware.json_backend: nawserver_bounded при MCP+backend, none при off;
mcp_enabled — boolean. C# модель сообщает system_text_json / true.

Host 135 core + 33 demo checks, C# direct 84 и полный HTTP suite 149 — PASS.
Frontend 18 tests и три TypeScript/Vite builds — PASS. GUI на модели подтвердил
односекундный pulse, автоматическое завершение, status/stop, состояние на прежней
странице экспериментов и System.Text.Json в INFO. Новая модель:
http://127.0.0.1:8800/#mcp. Старые процессы оставлены.

Далее — измерения именно этого расширения. SDK, пакеты и окружение не обновлялись.
<!-- backend26-19-1B3F8D4A19C6-5EAACE099A8B -->
### 2026-10-26 16:01:47 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 19

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/19`; SDKCONFIG SHA256-prefix `1B3F8D4A19C6`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
```

BIN: **1395792 байт**; linker total_size: **1395669 байт**; статические data+bss: **74728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 893374 | 0 | 0 |
| Flash Data | 354876 | 0 | 0 |
| DIRAM | 178243 | 341760 | 163517 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-20-EDE5E7997353-BB5665A76C14 -->
### 2026-10-26 16:01:47 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 20

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/20`; SDKCONFIG SHA256-prefix `EDE5E7997353`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
```

BIN: **1192464 байт**; linker total_size: **1192092 байт**; статические data+bss: **72456 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1044100 | 0 | 0 |
| DIRAM | 198628 | 452112 | 253484 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-27-50E66153B016-66297E662E18 -->
### 2026-10-26 16:01:47 +05:00 — NaWServer_showcase, esp32c3, SDK 5.5.6, профиль 27

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/27`; SDKCONFIG SHA256-prefix `50E66153B016`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1251424 байт**; linker total_size: **1251064 байт**; статические data+bss: **67804 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 772248 | 0 | 0 |
| Flash Data | 361256 | 0 | 0 |
| DRAM | 165964 | 321296 | 155332 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-28-CBB7A2686929-1A35BAE14C07 -->
### 2026-10-26 16:01:48 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 28

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/28`; SDKCONFIG SHA256-prefix `CBB7A2686929`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1263952 байт**; linker total_size: **1263576 байт**; статические data+bss: **69184 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 790196 | 0 | 0 |
| Flash Data | 368532 | 0 | 0 |
| DRAM | 154372 | 321296 | 166924 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-29-971EAB16DACF-1A5EF9605FFF -->
### 2026-10-26 16:01:48 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 29

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/29`; SDKCONFIG SHA256-prefix `971EAB16DACF`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1527792 байт**; linker total_size: **1527673 байт**; статические data+bss: **74880 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 914570 | 0 | 0 |
| Flash Data | 465800 | 0 | 0 |
| DIRAM | 178271 | 341760 | 163489 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-30-98C1E4C2863E-AEC119165CCC -->
### 2026-10-26 16:01:48 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 30

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/30`; SDKCONFIG SHA256-prefix `98C1E4C2863E`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1325376 байт**; linker total_size: **1325022 байт**; статические data+bss: **76728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1175188 | 0 | 0 |
| DIRAM | 202678 | 452112 | 249434 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-31-A86A891A3A94-C033BB90DF70 -->
### 2026-10-26 16:01:49 +05:00 — NaWServer_minimal, esp32c3, SDK 5.5.6, профиль 31

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/31`; SDKCONFIG SHA256-prefix `A86A891A3A94`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **992240 байт**; linker total_size: **991876 байт**; статические data+bss: **52612 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 733762 | 0 | 0 |
| Flash Data | 160332 | 0 | 0 |
| DRAM | 135990 | 321296 | 185306 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-32-72EA6F6FFD8E-074497EC70BB -->
### 2026-10-26 16:01:49 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 32

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/32`; SDKCONFIG SHA256-prefix `72EA6F6FFD8E`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1005648 байт**; linker total_size: **1005272 байт**; статические data+bss: **53456 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 750508 | 0 | 0 |
| Flash Data | 167756 | 0 | 0 |
| DRAM | 126284 | 321296 | 195012 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-32-72EA6F6FFD8E-A3A3042CF6D4 -->
### 2026-10-26 16:02:35 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 32

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/32`; SDKCONFIG SHA256-prefix `72EA6F6FFD8E`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1007760 байт**; linker total_size: **1007386 байт**; статические data+bss: **54480 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 751638 | 0 | 0 |
| Flash Data | 167652 | 0 | 0 |
| DRAM | 127372 | 321296 | 193924 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### Финальные MCP-on runtime и проверка соответствия ELF

```json
[
  {
    "board": "c6",
    "base": "http://192.0.2.1",
    "profile": 30,
    "measured_utc": "2026-10-26T11:04:36.951Z",
    "elf_sha256": "90c23eebdb1457957cd9cf3a804d807256f54d32bc5a00f2c1579feb7f6d3991",
    "info": {
      "v": 2,
      "source": "device",
      "boot_id": "c6-6ff00cab-8fb67e17",
      "chip": {
        "model": "ESP32-C6",
        "revision_major": 0,
        "revision_minor": 1,
        "cores": 1,
        "psram": false,
        "features": 82
      },
      "firmware": {
        "name": "NaWServer_c6_lcd147",
        "version": "0.1.0-c6-demo",
        "idf_version": "5.5.6",
        "json_backend": "nawserver_bounded",
        "mcp_enabled": true,
        "http_backend": "tcp",
        "board_label": "Waveshare C6 LCD 1.47",
        "build_date": "Oct 26 2026",
        "build_time": "13:16:07",
        "reset_reason": 11
      },
      "capabilities": {
        "tasks": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "memory": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "experiments": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "led": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "connections": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "session_details_not_exposed"
        },
        "simulator_control": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "device"
        },
        "sse": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "authenticated_ws": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "pre_handshake_auth_unavailable"
        }
      },
      "details": {
        "hardware": {
          "chip_model": "ESP32-C6",
          "architecture": "RISC-V",
          "cpu_mhz": 160,
          "crystal_mhz": 40,
          "apb_mhz": 40,
          "revision_major": 0,
          "revision_minor": 1,
          "cores": 1,
          "wifi_24ghz": true,
          "wifi_6": true,
          "bluetooth_le": true,
          "bluetooth_classic": false,
          "ieee802154": true,
          "usb_serial_jtag": true,
          "hp_sram_bytes": 524288,
          "lp_sram_bytes": 16384
        },
        "flash_memory": {
          "flash_bytes": 4194304,
          "flash_jedec_id": "0x204016",
          "embedded_flash": true,
          "heap_internal_bytes": 335212,
          "heap_dma_bytes": 318884,
          "psram_bytes": 0,
          "app_partition": "factory",
          "app_partition_bytes": 3145728
        },
        "mac_addresses": {
          "factory_mac": "DEVICE-MAC",
          "custom_mac": null,
          "base_mac": "DEVICE-MAC",
          "wifi_sta_mac": "DEVICE-MAC",
          "wifi_ap_mac": "DEVICE-MAC",
          "bluetooth_mac": "DEVICE-MAC",
          "ethernet_mac": "DEVICE-MAC",
          "ieee802154_eui64": "DEVICE-MAC:1F:5C"
        },
        "network": {
          "wifi_connected": true,
          "wifi_mode": "STA",
          "ipv4": "192.0.2.1",
          "netmask": "255.255.240.0",
          "gateway": "192.0.2.4",
          "ssid": "<SSID>",
          "ap_bssid": "DEVICE-MAC",
          "channel": 6,
          "rssi_dbm": -80,
          "wifi_auth_mode": 7
        },
        "board": {
          "lcd_controller": "ST7789T",
          "lcd_resolution": "172 x 320",
          "lcd_spi_ready": true,
          "lcd_visual_status": "unverified",
          "backlight_percent": 30,
          "rgb_count": 1,
          "rgb_gpio": 8,
          "rgb_order": "RGB",
          "rgb_driver": "RMT",
          "sd_slot": true,
          "sd_mounted": false,
          "touch": false,
          "ble_started": false
        },
        "build": {
          "project": "NaWServer_c6_lcd147",
          "version": "0.1.0-c6-demo",
          "idf_version": "5.5.6",
          "build_date": "Oct 26 2026",
          "build_time": "13:16:07",
          "secure_version": 0,
          "elf_sha256": "90c23eebdb1457957cd9cf3a804d807256f54d32bc5a00f2c1579feb7f6d3991",
          "reset_reason": 11
        }
      },
      "limits": {
        "frame_bytes": 4096,
        "body_bytes": 4096,
        "task_page_max": 8,
        "max_ws_clients": 4
      }
    },
    "summary": {
      "v": 2,
      "source": "device",
      "boot_id": "c6-6ff00cab-8fb67e17",
      "seq": 2056,
      "captured_ms": 205994,
      "data": {
        "uptime_ms": 205994,
        "sample_interval_ms": 1000,
        "cpu_busy_pct": 16.4923507649235,
        "core_busy_pct": [
          16.4923507649235
        ],
        "memory": [
          {
            "id": "internal",
            "label": "Внутренняя RAM",
            "total_bytes": 335212,
            "free_bytes": 185248,
            "min_free_bytes": 160988,
            "largest_block_bytes": 159744
          }
        ],
        "rssi_dbm": -77,
        "task_count": 12,
        "ws_clients": 2,
        "network_status": "connected",
        "counters": {
          "http_requests": 359,
          "ws_rx_frames": 0,
          "ws_tx_frames": 187,
          "ws_rx_bytes": 0,
          "ws_tx_bytes": 170075,
          "rejected": 22
        },
        "experiment": {
          "running": false,
          "kind": "pulse",
          "duration_ms": 3000,
          "on_ms": 20,
          "period_ms": 1000,
          "core": 0,
          "priority": 1,
          "elapsed_ms": 401,
          "progress_pct": 13.366666666666667,
          "pi": 3.1412921728273475,
          "iterations": 3328,
          "active": false,
          "sample_ms": 100,
          "enabled": false
        },
        "led": {
          "supported": false,
          "enabled": false,
          "period_ms": 1000,
          "duty_pct": 50,
          "gpio": -1,
          "active_low": true,
          "level": false,
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "available_drivers": [
            "rmt"
          ],
          "driver": "rmt",
          "requested_driver": "rmt",
          "driver_error": "",
          "supported": true,
          "enabled": true,
          "gpio": 8,
          "count": 1,
          "mode": "solid",
          "color": 16752688,
          "brightness_pct": 20,
          "period_ms": 1000,
          "pixels": [
            3350537
          ]
        }
      }
    }
  },
  {
    "board": "s3",
    "base": "http://192.0.2.2",
    "profile": 29,
    "measured_utc": "2026-10-26T11:04:37.512Z",
    "elf_sha256": "63d57bfa50f688b54f8debf18e87037d3b99d690505900cb481abf8c73177047",
    "info": {
      "v": 2,
      "source": "device",
      "boot_id": "s3-d4859290-7dc13e03",
      "chip": {
        "model": "ESP32-S3",
        "revision_major": 0,
        "revision_minor": 2,
        "cores": 2,
        "psram": true,
        "features": 18
      },
      "firmware": {
        "name": "NaWServer_touch_lcd7",
        "version": "0.1.0-touch-lcd7",
        "idf_version": "5.5.6",
        "json_backend": "nawserver_bounded",
        "mcp_enabled": true,
        "http_backend": "tcp",
        "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
        "build_date": "Oct 26 2026",
        "build_time": "13:16:05",
        "reset_reason": 1
      },
      "capabilities": {
        "tasks": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "memory": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "experiments": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "led": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "connections": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "session_details_not_exposed"
        },
        "simulator_control": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "device"
        },
        "sse": {
          "supported": true,
          "origin": "device",
          "device_status": "implemented_unverified"
        },
        "authenticated_ws": {
          "supported": false,
          "origin": "device",
          "device_status": "unsupported",
          "reason": "pre_handshake_auth_unavailable"
        }
      },
      "limits": {
        "frame_bytes": 4096,
        "body_bytes": 4096,
        "task_page_max": 8,
        "max_ws_clients": 4
      },
      "details": {
        "hardware": {
          "chip_model": "ESP32-S3",
          "architecture": "Xtensa LX7",
          "cpu_mhz": 240,
          "crystal_mhz": 40,
          "apb_mhz": 80,
          "revision_major": 0,
          "revision_minor": 2,
          "cores": 2,
          "wifi_24ghz": true,
          "wifi_6": false,
          "bluetooth_le": true,
          "bluetooth_classic": false,
          "ieee802154": false,
          "usb_serial_jtag": true,
          "hp_sram_bytes": 524288,
          "lp_sram_bytes": 16384
        },
        "flash_memory": {
          "flash_bytes": 16777216,
          "flash_jedec_id": "0xC84018",
          "embedded_flash": false,
          "heap_internal_bytes": 284059,
          "heap_dma_bytes": 275891,
          "psram_bytes": 7005280,
          "app_partition": "factory",
          "app_partition_bytes": 5242880
        },
        "mac_addresses": {
          "factory_mac": "DEVICE-MAC",
          "custom_mac": null,
          "base_mac": "DEVICE-MAC",
          "wifi_sta_mac": "DEVICE-MAC",
          "wifi_ap_mac": "DEVICE-MAC",
          "bluetooth_mac": "DEVICE-MAC",
          "ethernet_mac": "DEVICE-MAC",
          "ieee802154_eui64": null
        },
        "network": {
          "wifi_connected": true,
          "wifi_mode": "STA",
          "ipv4": "192.0.2.2",
          "netmask": "255.255.240.0",
          "gateway": "192.0.2.4",
          "ssid": "<SSID>",
          "ap_bssid": "DEVICE-MAC",
          "channel": 6,
          "rssi_dbm": -71,
          "wifi_auth_mode": 7
        },
        "board": {
          "lcd_interface": "RGB565",
          "lcd_resolution": "800 x 480",
          "lcd_ready": true,
          "backlight_enabled": true,
          "touch": true,
          "touch_controller": "GT911",
          "renderer": "LVGL 8.3.11",
          "framebuffers": 2,
          "rgb_count": 10,
          "rgb_gpio": 6,
          "rgb_order": "GRB",
          "rgb_driver": "RMT",
          "rgb_ready": true,
          "ble_started": false
        },
        "build": {
          "project": "NaWServer_touch_lcd7",
          "version": "0.1.0-touch-lcd7",
          "idf_version": "5.5.6",
          "build_date": "Oct 26 2026",
          "build_time": "13:16:05",
          "secure_version": 0,
          "elf_sha256": "63d57bfa50f688b54f8debf18e87037d3b99d690505900cb481abf8c73177047",
          "reset_reason": 1
        }
      }
    },
    "summary": {
      "v": 2,
      "source": "device",
      "boot_id": "s3-d4859290-7dc13e03",
      "seq": 1516,
      "captured_ms": 151564,
      "data": {
        "uptime_ms": 151564,
        "sample_interval_ms": 1000,
        "cpu_busy_pct": 5.210343689689312,
        "core_busy_pct": [
          10.068697939061833,
          0.35198944031679114
        ],
        "memory": [
          {
            "id": "internal",
            "label": "Внутренняя RAM",
            "total_bytes": 284059,
            "free_bytes": 36935,
            "min_free_bytes": 8096,
            "largest_block_bytes": 21504
          },
          {
            "id": "psram",
            "label": "PSRAM",
            "total_bytes": 7005280,
            "free_bytes": 5461068,
            "min_free_bytes": 5461068,
            "largest_block_bytes": 5373952
          }
        ],
        "rssi_dbm": -71,
        "task_count": 16,
        "ws_clients": 1,
        "network_status": "connected",
        "counters": {
          "http_requests": 191,
          "ws_rx_frames": 0,
          "ws_tx_frames": 116,
          "ws_rx_bytes": 0,
          "ws_tx_bytes": 162025,
          "rejected": 10
        },
        "experiment": {
          "running": false,
          "kind": "pulse",
          "duration_ms": 3000,
          "on_ms": 20,
          "period_ms": 1000,
          "core": 0,
          "priority": 1,
          "elapsed_ms": 300,
          "progress_pct": 10,
          "pi": 3.141313635738085,
          "iterations": 3584,
          "active": false,
          "sample_ms": 100,
          "enabled": false
        },
        "led": {
          "supported": false,
          "enabled": false,
          "period_ms": 1000,
          "duty_pct": 50,
          "gpio": -1,
          "active_low": true,
          "level": false,
          "reason": "board_led_not_confirmed"
        },
        "strip": {
          "driver": "rmt",
          "requested_driver": "rmt",
          "driver_error": "",
          "supported": true,
          "enabled": false,
          "gpio": 6,
          "count": 10,
          "mode": "solid",
          "color": 16752688,
          "brightness_pct": 20,
          "period_ms": 1000,
          "pixels": [
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0
          ]
        },
        "display": {
          "ready": true,
          "width": 800,
          "height": 480,
          "pattern": 0,
          "backlight": true,
          "pressed": false,
          "x": 0,
          "y": 0,
          "touches": 0,
          "touch_errors": 0,
          "renderer": "lvgl-8.3.11",
          "framebuffers": 2,
          "frame_swaps": 2,
          "frame_events": 4423,
          "vsyncs": 4423
        },
        "network": {
          "socket_limit": 24,
          "tcp_pcb_limit": 32,
          "reserved_sockets": 3,
          "httpd_limit": 21,
          "httpd_open": 3,
          "httpd_free": 18,
          "http_reserve": 3,
          "ws_limit": 4,
          "ws_clients": 1,
          "sse_limit": 4,
          "sse_clients": 1,
          "admission_rejected": 0,
          "last_rejection": "none",
          "retry_after_ms": 2000,
          "captured_ms": 151634,
          "ready": true
        }
      }
    }
  }
]
```

### Runtime C6 — 2026-10-26T11:04:37.578Z (UTC)

Подключённая плата, http://192.0.2.1. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "c6",
  "base": "http://192.0.2.1",
  "measured_utc": "2026-10-26T11:04:37.578Z",
  "checks": 27,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 206796,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 335212,
          "free_bytes": 180940,
          "min_free_bytes": 160988,
          "largest_block_bytes": 159744
        }
      ]
    },
    {
      "stage": "after_ws_and_sse_probes",
      "captured_ms": 216994,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 335212,
          "free_bytes": 181508,
          "min_free_bytes": 154632,
          "largest_block_bytes": 159744
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-C6",
    "revision_major": 0,
    "revision_minor": 1,
    "cores": 1,
    "psram": false,
    "features": 82
  },
  "firmware": {
    "name": "NaWServer_c6_lcd147",
    "version": "0.1.0-c6-demo",
    "idf_version": "5.5.6",
    "json_backend": "nawserver_bounded",
    "mcp_enabled": true,
    "http_backend": "tcp",
    "board_label": "Waveshare C6 LCD 1.47",
    "build_date": "Oct 26 2026",
    "build_time": "13:16:07",
    "reset_reason": 11
  },
  "source": "device",
  "probe_errors": [],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "c6-6ff00cab-8fb67e17",
    "seq": 2161,
    "captured_ms": 216495,
    "cpu_busy_pct": 47.3,
    "free_bytes": 181508,
    "task_count": 12
  },
  "status": "PASS"
}
```

### Runtime S3 — 2026-10-26T11:04:48.079Z (UTC)

Подключённая плата, http://192.0.2.2. Короткий функциональный прогон; это не soak/leak-тест.

```json
{
  "tag": "s3",
  "base": "http://192.0.2.2",
  "measured_utc": "2026-10-26T11:04:48.079Z",
  "checks": 7,
  "snapshots": [
    {
      "stage": "connected_before_probes",
      "captured_ms": 162364,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284059,
          "free_bytes": 35051,
          "min_free_bytes": 8016,
          "largest_block_bytes": 16384
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7005280,
          "free_bytes": 5461068,
          "min_free_bytes": 5461068,
          "largest_block_bytes": 5373952
        }
      ]
    }
  ],
  "chip": {
    "model": "ESP32-S3",
    "revision_major": 0,
    "revision_minor": 2,
    "cores": 2,
    "psram": true,
    "features": 18
  },
  "firmware": {
    "name": "NaWServer_touch_lcd7",
    "version": "0.1.0-touch-lcd7",
    "idf_version": "5.5.6",
    "json_backend": "nawserver_bounded",
    "mcp_enabled": true,
    "http_backend": "tcp",
    "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
    "build_date": "Oct 26 2026",
    "build_time": "13:16:05",
    "reset_reason": 1
  },
  "source": "device",
  "probe_errors": [
    "Error: WS connection error: ",
    "AbortError: This operation was aborted"
  ],
  "sse": {
    "v": 2,
    "source": "device",
    "boot_id": "s3-d4859290-7dc13e03",
    "seq": 1751,
    "captured_ms": 175065,
    "cpu_busy_pct": 5.5,
    "free_bytes": 26575,
    "task_count": 16
  },
  "status": "FAIL",
  "error": "Error: /api/v2/tasks?offset=0&limit=8: HTTP 408"
}
```

### Итог проверки отдельной страницы и записи через MCP

Все шесть MCP SDK-профилей 27–32 собраны после исправления readonly Minimal.
Профили native HTTPD/MCP-off C6 и S3 также собраны с новой INFO metadata.
Одна промежуточная S3-сборка без component manager завершилась ошибкой
esp_lcd_touch.h; повтор с прежним cached manager успешен, загрузок/обновлений нет.
Неполный первый host CTest запуск не нашёл mcp_disabled_probe.exe; после сборки
этой цели финальный CTest3/3 PASS (135 core +33 demo checks).

| Финальный профиль | BIN, байт | data+bss, байт |
|---|---:|---:|
| showcase C3 TCP/MCP, IDF5.5.6 | 1251424 | 67804 |
| showcase C3 TCP/MCP, IDF6.1.0 | 1263952 | 69184 |
| S3 TCP/MCP, IDF5.5.6 | 1527792 | 74880 |
| C6 TCP/MCP, IDF5.5.6 | 1325376 | 76728 |
| Minimal C3 TCP/MCP, IDF5.5.6 | 992240 | 52612 |
| Minimal C3 TCP/MCP, IDF6.1.0 | 1007760 | 54480 |

Сопоставление итогового ELF с INFO — MATCH на обеих платах:
C6 90c23eebdb1457957cd9cf3a804d807256f54d32bc5a00f2c1579feb7f6d3991;
S3 63d57bfa50f688b54f8debf18e87037d3b99d690505900cb481abf8c73177047.
Runtime snapshot выше: C6 internal free185248/min160988 B; S3 free36935/min8096 B,
PSRAM free5461068 B. Это снимки в текущем сеансе, не matched-load benchmark.
Рост Flash включает новый интерфейс, описания и сериализацию результата
эксперимента с вещественными значениями; чистая цена одного MCP не измерена.

C6: modern MCP smoke, start/status/REST readback/stop,401/invalid-preserves-state,
metadata и все27 прежних HTTP/WS/SSE проверок — PASS. GUI подтвердил четыре
инструмента, experiment_status и INFO NaWServer bounded JSON / MCP включён / TCP.

S3: modern smoke и отдельный live start/status/REST readback/stop прогон PASS;
401 и неверный duration проверены, JSON metadata правильная. Однако ранее
получены408 при tools/list и таймаут Node5s; финальная GUI проверка снова408
на experiment_status, а общий board probe остановился после7 checks на
GET /api/v2/tasks?offset=0&limit=8 с408. Эти отказы не считаются исправленными.
25-секундный UART capture не показал panic/stack overflow/Wi-Fi disconnect;
имеется прежнее сообщение system_api о MAC address generation.

Generic408 'HTTP request rejected' формируется при чтении заголовков до MCP,
Basic guard и experiment callback. Подтверждена архитектурная возможность
задержки единственного HTTP worker пустыми accepted/preconnect соединениями;
фактический источник конкретного408 без дополнительной трассировки не доказан.
Таймауты не увеличивались, автоматические повторы управляющих команд не добавлены.
Полная стабильность TCP/S3 остаётся открытой; GUI показывает ошибку честно.

Нагрузки ограничены короткими pulse на1–3секунды; длительный soak не запускался.
Новый рабочий адрес модели: http://127.0.0.1:8800/#mcp (зарегистрирован в портале).
Коммит/пуш не делались. git diff --check PASS.

## 2026-10-26 — понятная структура страницы MCP

По замечанию владельца кнопка получения списка перенесена непосредственно
после заголовка «Инструменты MCP». Полная подпись «Получить список инструментов»,
видимая рамка, заливка и жирный текст; не отодвигается к правому краю панели.
Сервер, JSON-библиотека и учётная запись оформлены отдельными выровненными строками.
Рабочий порядок обозначен: 1. Список инструментов; 2. Аргументы JSON;
3. Полученные данные. Кнопка выполнения подписана «Выполнить инструмент».

Логика MCP, авторизация и runtime экспериментов не менялись. TypeScript/Vite
три профиля PASS; frontend tests18/18 PASS. На живом8800/#mcp визуально проверены
кнопка непосредственно после заголовка, структура сведений и получение4tools.
Снимок local/mcp-structured-ui.png (ignored). Далее размеры обновлённых образов.
<!-- backend26-29-971EAB16DACF-E4ACCDA39B12 -->
### 2026-10-26 16:13:20 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 29

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/29`; SDKCONFIG SHA256-prefix `971EAB16DACF`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1528096 байт**; linker total_size: **1527977 байт**; статические data+bss: **74880 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 914570 | 0 | 0 |
| Flash Data | 466104 | 0 | 0 |
| DIRAM | 178271 | 341760 | 163489 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-30-98C1E4C2863E-6A02EE01742E -->
### 2026-10-26 16:13:21 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 30

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/30`; SDKCONFIG SHA256-prefix `98C1E4C2863E`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
```

BIN: **1325696 байт**; linker total_size: **1325334 байт**; статические data+bss: **76728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1175500 | 0 | 0 |
| DIRAM | 202678 | 452112 | 249434 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


Итог UI-правки: C6 BIN1325696 B/data+bss76728 B; S3 BIN1528096 B/data+bss74880 B.
Оба образа записаны после проверки chip/MAC и диапазонов, vendor verify PASS.
C6 ответила, INFO ELF соответствует локальному финальному образу. На S3
контрольный HTTP-запрос после прошивки завершился закрытием сокета/таймаутом;
повтор через локальный Wi-Fi interface192.168.16.40 также timeout8s. Живой
HTTP результат S3 на этом этапе не подтверждён; прежняя проблема транспорта
не объявлена исправленной. Локальная8800 демка: список4tools и device_info PASS,
финальный снимок показывает кнопку сразу после заголовка и заполненный результат.

### 2026-10-26 — собственная иконка MCP

Добавлен локальный SVG mcp.svg: центральный модуль, входящая стрелка и три
подключённых узла. Применён в меню MCP и заголовке «Инструменты MCP» через общий
Icon.vue; цвет наследуется от темы. Три TypeScript/Vite профиля собраны,
иконка визуально проверена на8800/#mcp; assets упакованы для трёх ESP демо.
SDK/окружение не обновлялись, платы в рамках этой небольшой правки не прошивались.

## 2026-10-26 — настройки JSON/MCP и сменные reader backend

Поручение владельца: вынести размеры и ограничения в настройки с комментариями,
сделать абстракцию, выбрать первую альтернативную библиотеку и затем измерить.
Первый адаптер — yyjson0.13.0, commit6447536015f3d600f3d65323b10976103b337ca7.
Unmodified upstream .h/.c +MIT license сохранены в third_party/yyjson; SDK,
компиляторы, драйверы, npm/PlatformIO packages не устанавливались и не обновлялись.

Общий API: opaque Value / Document поверх caller-owned byte arena; никакой
Token pointer arithmetic или второго нормализованного DOM. Bounded сохраняет
borrowed immutable input, yyjson хранит native decoded DOM в fixed pool без
heap fallback. Writer общий и ограниченный; выбранный reader проверяет результат.
Числовые lexemes проходят общую checked charconv-проверку: int64/uint64 точные,
overflow/underflow отвергаются. ID строки сохраняют значение, но не обещают
тот же выбор Unicode escape spelling. Другие MCP/CBOR операции не менялись.

Define/Kconfig: JSON_INPUT_BYTES4096, STRING_BYTES512, CONTAINER_ITEMS32, DEPTH8,
VALUE_CAPACITY256, WORKSPACE_BYTES2048(bounded)/16384(yyjson), MCP_OUTPUT_BYTES2048.
Область reader и полный output отдельные. Kconfig choice включает ровно один
reader; неопределённый/двойной выбор через compiler flags — ошибка. MCP-off не
требует JSON и не компилирует его implementation/буферы. На S3 одна PSRAM allocation.
В host и IDF CMake доступен NaWServer_YYJSON_TARGET для существующего target,
чтобы не подключать вторую копию совместимой библиотеки приложения.

Host common corpus на frozen checkpoint:154 core +33 demo checks, отдельные
nondefault resource tests (8192input/1024string/12depth/128values/32768arena),
MCP-off probe и8 negative configure checks. Выявленная при параллельной разработке
ошибка lifetime при workspace reuse исправлена: fixed-pool Document destructor
не разыменовывает уже переиспользованный yyjson_doc. Финальные оба профиля4/4PASS.
Первый SDK compile остановился на -Werror=parentheses в четырёх ternary/assignment;
заменено явными if без изменения результата. Эти попытки не считаются PASS.

Предварительный host benchmark (MSVC19.51, шесть неизменных fixtures,
200итераций/1200parses; тайминг не переносится на ESP):
```json
{"backend":"nawserver_bounded","ok":true,"reserved_arena_bytes":2048,"reader_parse_peak_bytes":1016,"maximum_value_count":127,"output_reserved_bytes":2048,"maximum_encoded_output_bytes":1109,"fixtures":6,"iterations":200,"parses":1200,"elapsed_us":11293,"existing_application_yyjson":false,"checksum":257600}
{"backend":"nawserver_bounded","ok":true,"reserved_arena_bytes":2048,"reader_parse_peak_bytes":1016,"maximum_value_count":127,"output_reserved_bytes":2048,"maximum_encoded_output_bytes":1109,"fixtures":6,"iterations":200,"parses":1200,"elapsed_us":12338,"existing_application_yyjson":true,"checksum":723800}
{"backend":"yyjson","ok":true,"reserved_arena_bytes":16384,"reader_parse_peak_bytes":4288,"maximum_value_count":127,"output_reserved_bytes":2048,"maximum_encoded_output_bytes":1109,"fixtures":6,"iterations":200,"parses":1200,"elapsed_us":8319,"existing_application_yyjson":false,"checksum":257600}
{"backend":"yyjson","ok":true,"reserved_arena_bytes":16384,"reader_parse_peak_bytes":4288,"maximum_value_count":127,"output_reserved_bytes":2048,"maximum_encoded_output_bytes":1109,"fixtures":6,"iterations":200,"parses":1200,"elapsed_us":10041,"existing_application_yyjson":true,"checksum":723800}
```


### JSON benchmark C6, профиль 36

```json
{
  "profile": 36,
  "measured_utc": "2026-10-26T11:54:38.1341941Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "nawserver_bounded",
    "ok": true,
    "reserved_arena_bytes": 2048,
    "reader_parse_peak_bytes": 1016,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 333403,
    "existing_application_yyjson": false,
    "checksum": 257600
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=463316 minimum=463316 stack_free=6908\r",
    "JSON_BENCH_DONE result=0 heap_after=463316 heap_minimum=463316 stack_free=6272\r"
  ]
}
```
<!-- backend26-28-B63B22487D6B-9795F283BCAE -->
### 2026-10-26 16:55:28 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 28

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/28`; SDKCONFIG SHA256-prefix `B63B22487D6B`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1264320 байт**; linker total_size: **1263942 байт**; статические data+bss: **69216 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 792090 | 0 | 0 |
| Flash Data | 369052 | 0 | 0 |
| DRAM | 154404 | 321296 | 166892 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-29-BD7604C3B4FC-818B17DA0D33 -->
### 2026-10-26 16:55:29 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 29

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/29`; SDKCONFIG SHA256-prefix `BD7604C3B4FC`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1530752 байт**; linker total_size: **1530629 байт**; статические data+bss: **74896 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 917014 | 0 | 0 |
| Flash Data | 466312 | 0 | 0 |
| DIRAM | 178287 | 341760 | 163473 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-30-60B7B92DAACF-C4AB56E49B07 -->
### 2026-10-26 16:55:30 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 30

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/30`; SDKCONFIG SHA256-prefix `60B7B92DAACF`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1326192 байт**; linker total_size: **1325836 байт**; статические data+bss: **76752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1178050 | 0 | 0 |
| DIRAM | 202702 | 452112 | 249410 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-33-85710B71A796-89D7E2B6E578 -->
### 2026-10-26 16:55:30 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 33

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/33`; SDKCONFIG SHA256-prefix `85710B71A796`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1442176 байт**; linker total_size: **1441822 байт**; статические data+bss: **91088 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1294036 | 0 | 0 |
| DIRAM | 217038 | 452112 | 235074 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-34-2143FB55A310-8411F207E553 -->
### 2026-10-26 16:55:31 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 34

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/34`; SDKCONFIG SHA256-prefix `2143FB55A310`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1641216 байт**; linker total_size: **1641093 байт**; статические data+bss: **74896 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1014502 | 0 | 0 |
| Flash Data | 479288 | 0 | 0 |
| DIRAM | 178287 | 341760 | 163473 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-35-69656BF5E81B-11C600278F70 -->
### 2026-10-26 16:55:31 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 35

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/35`; SDKCONFIG SHA256-prefix `69656BF5E81B`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1362112 байт**; linker total_size: **1361744 байт**; статические data+bss: **83552 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 876996 | 0 | 0 |
| Flash Data | 381948 | 0 | 0 |
| DRAM | 168740 | 321296 | 152556 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-36-06F5222B2646-8A6E926DF645 -->
### 2026-10-26 16:55:32 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 36

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/36`; SDKCONFIG SHA256-prefix `06F5222B2646`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **285344 байт**; linker total_size: **284198 байт**; статические data+bss: **13416 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 236336 | 0 | 0 |
| DIRAM | 56134 | 452112 | 395978 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-37-042112F0D0B0-A991C898501B -->
### 2026-10-26 16:55:32 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 37

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/37`; SDKCONFIG SHA256-prefix `042112F0D0B0`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **400480 байт**; linker total_size: **400142 байт**; статические data+bss: **27752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 352266 | 0 | 0 |
| DIRAM | 70484 | 452112 | 381628 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### JSON benchmark C6, профиль 37

```json
{
  "profile": 37,
  "measured_utc": "2026-10-26T11:55:35.7422452Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "yyjson",
    "ok": true,
    "reserved_arena_bytes": 16384,
    "reader_parse_peak_bytes": 4216,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 244393,
    "existing_application_yyjson": false,
    "checksum": 257600
  },
  "heap_and_stack": [
    "JSON_BENCH_DONE result=0 heap_after=448964 heap_minimum=448964 stack_free=6328\r"
  ]
}
```

### JSON benchmark C6, профиль 38

```json
{
  "profile": 38,
  "measured_utc": "2026-10-26T11:57:51.2490373Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "nawserver_bounded",
    "ok": true,
    "reserved_arena_bytes": 2048,
    "reader_parse_peak_bytes": 1016,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 432423,
    "existing_application_yyjson": true,
    "checksum": 723800
  },
  "heap_and_stack": [
    "JSON_BENCH_DONE result=0 heap_after=463300 heap_minimum=459136 stack_free=6224\r"
  ]
}
```
<!-- backend26-38-06F5222B2646-EED5CB282660 -->
### 2026-10-26 16:59:05 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 38

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/38`; SDKCONFIG SHA256-prefix `06F5222B2646`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **401232 байт**; linker total_size: **400900 байт**; статические data+bss: **13416 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 353024 | 0 | 0 |
| DIRAM | 56148 | 452112 | 395964 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-39-042112F0D0B0-F0DFB400EC3B -->
### 2026-10-26 16:59:05 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 39

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/39`; SDKCONFIG SHA256-prefix `042112F0D0B0`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **400624 байт**; linker total_size: **400298 байт**; статические data+bss: **27752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 352422 | 0 | 0 |
| DIRAM | 70484 | 452112 | 381628 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### JSON benchmark C6, профиль 39

```json
{
  "profile": 39,
  "measured_utc": "2026-10-26T11:59:12.9699526Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "yyjson",
    "ok": true,
    "reserved_arena_bytes": 16384,
    "reader_parse_peak_bytes": 4216,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 331877,
    "existing_application_yyjson": true,
    "checksum": 723800
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=448964 minimum=448964 stack_free=6908",
    "JSON_BENCH_DONE result=0 heap_after=448964 heap_minimum=444800 stack_free=6328"
  ]
}
```
<!-- backend26-28-B63B22487D6B-56A6085D2B9E -->
### 2026-10-26 17:05:56 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 28

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/28`; SDKCONFIG SHA256-prefix `B63B22487D6B`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1264048 байт**; linker total_size: **1263680 байт**; статические data+bss: **69216 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 791588 | 0 | 0 |
| Flash Data | 369292 | 0 | 0 |
| DRAM | 154404 | 321296 | 166892 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-29-BD7604C3B4FC-4291B5829F7D -->
### 2026-10-26 17:05:57 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 29

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/29`; SDKCONFIG SHA256-prefix `BD7604C3B4FC`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1530288 байт**; linker total_size: **1530169 байт**; статические data+bss: **74896 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 916330 | 0 | 0 |
| Flash Data | 466536 | 0 | 0 |
| DIRAM | 178287 | 341760 | 163473 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-30-60B7B92DAACF-F78C42111BA5 -->
### 2026-10-26 17:05:57 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 30

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/30`; SDKCONFIG SHA256-prefix `60B7B92DAACF`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1325856 байт**; linker total_size: **1325496 байт**; статические data+bss: **76752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1177710 | 0 | 0 |
| DIRAM | 202702 | 452112 | 249410 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-33-85710B71A796-95CE693E0A2C -->
### 2026-10-26 17:05:57 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 33

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/33`; SDKCONFIG SHA256-prefix `85710B71A796`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1442032 байт**; linker total_size: **1441676 байт**; статические data+bss: **91088 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1293890 | 0 | 0 |
| DIRAM | 217038 | 452112 | 235074 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-34-2143FB55A310-6B6862AA18C4 -->
### 2026-10-26 17:05:58 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 34

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/34`; SDKCONFIG SHA256-prefix `2143FB55A310`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1640912 байт**; linker total_size: **1640793 байт**; статические data+bss: **74896 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1013978 | 0 | 0 |
| Flash Data | 479512 | 0 | 0 |
| DIRAM | 178287 | 341760 | 163473 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-36-06F5222B2646-86C5157A3421 -->
### 2026-10-26 17:05:58 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 36

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/36`; SDKCONFIG SHA256-prefix `06F5222B2646`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **285408 байт**; linker total_size: **284256 байт**; статические data+bss: **13416 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 236394 | 0 | 0 |
| DIRAM | 56134 | 452112 | 395978 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-37-042112F0D0B0-E4485E386E22 -->
### 2026-10-26 17:05:58 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 37

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/37`; SDKCONFIG SHA256-prefix `042112F0D0B0`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **400528 байт**; linker total_size: **400202 байт**; статические data+bss: **27752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 352326 | 0 | 0 |
| DIRAM | 70484 | 452112 | 381628 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-38-06F5222B2646-93E0B1DEE429 -->
### 2026-10-26 17:05:58 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 38

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/38`; SDKCONFIG SHA256-prefix `06F5222B2646`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **401296 байт**; linker total_size: **400958 байт**; статические data+bss: **13416 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 353082 | 0 | 0 |
| DIRAM | 56148 | 452112 | 395964 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-39-042112F0D0B0-A6F2003AC28A -->
### 2026-10-26 17:05:59 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 39

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/39`; SDKCONFIG SHA256-prefix `042112F0D0B0`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **400688 байт**; linker total_size: **400358 байт**; статические data+bss: **27752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 352482 | 0 | 0 |
| DIRAM | 70484 | 452112 | 381628 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### JSON benchmark C6, профиль 36

```json
{
  "profile": 36,
  "measured_utc": "2026-10-26T12:06:03.3483073Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "nawserver_bounded",
    "ok": true,
    "reserved_arena_bytes": 2048,
    "reader_parse_peak_bytes": 1016,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 334462,
    "existing_application_yyjson": false,
    "checksum": 257600
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=463316 minimum=463316 stack_free=6908",
    "JSON_BENCH_DONE result=0 heap_after=463316 heap_minimum=463316 stack_free=6672"
  ]
}
```

### JSON benchmark C6, профиль 37

```json
{
  "profile": 37,
  "measured_utc": "2026-10-26T12:06:21.6165806Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "yyjson",
    "ok": true,
    "reserved_arena_bytes": 16384,
    "reader_parse_peak_bytes": 4216,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 243340,
    "existing_application_yyjson": false,
    "checksum": 257600
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=448964 minimum=448964 stack_free=6908",
    "JSON_BENCH_DONE result=0 heap_after=448964 heap_minimum=448964 stack_free=6564"
  ]
}
```

### JSON benchmark C6, профиль 38

```json
{
  "profile": 38,
  "measured_utc": "2026-10-26T12:06:39.8933526Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "nawserver_bounded",
    "ok": true,
    "reserved_arena_bytes": 2048,
    "reader_parse_peak_bytes": 1016,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 426415,
    "existing_application_yyjson": true,
    "checksum": 723800
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=463300 minimum=463300 stack_free=6908",
    "JSON_BENCH_DONE result=0 heap_after=463300 heap_minimum=459136 stack_free=6528"
  ]
}
```

### JSON benchmark C6, профиль 39

```json
{
  "profile": 39,
  "measured_utc": "2026-10-26T12:06:58.0785670Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "yyjson",
    "ok": true,
    "reserved_arena_bytes": 16384,
    "reader_parse_peak_bytes": 4216,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 325047,
    "existing_application_yyjson": true,
    "checksum": 723800
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=448964 minimum=448964 stack_free=6908",
    "JSON_BENCH_DONE result=0 heap_after=448964 heap_minimum=444800 stack_free=6528"
  ]
}
```
<!-- backend26-31-483437576A82-055BEE9B9090 -->
### 2026-10-26 17:13:20 +05:00 — NaWServer_minimal, esp32c3, SDK 5.5.6, профиль 31

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/31`; SDKCONFIG SHA256-prefix `483437576A82`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **991568 байт**; linker total_size: **991208 байт**; статические data+bss: **52644 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 735210 | 0 | 0 |
| Flash Data | 160348 | 0 | 0 |
| DRAM | 135938 | 321296 | 185358 |
| RTC SLOW | 56 | 8192 | 8136 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-32-D0B4E776EE5B-7A32736F4978 -->
### 2026-10-26 17:13:21 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 32

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/32`; SDKCONFIG SHA256-prefix `D0B4E776EE5B`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1005680 байт**; linker total_size: **1005312 байт**; статические data+bss: **54512 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 751680 | 0 | 0 |
| Flash Data | 167668 | 0 | 0 |
| DRAM | 127320 | 321296 | 193976 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-35-69656BF5E81B-1F3B38CC17C2 -->
### 2026-10-26 17:13:21 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 35

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/35`; SDKCONFIG SHA256-prefix `69656BF5E81B`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1361984 байт**; linker total_size: **1361614 байт**; статические data+bss: **83552 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 876634 | 0 | 0 |
| Flash Data | 382180 | 0 | 0 |
| DRAM | 168740 | 321296 | 152556 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-30-11C65F72180C-00CC72B6E770 -->
### 2026-10-26 17:31:55 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 30

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/30`; SDKCONFIG SHA256-prefix `11C65F72180C`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1325856 байт**; linker total_size: **1325492 байт**; статические data+bss: **76752 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1177706 | 0 | 0 |
| DIRAM | 202702 | 452112 | 249410 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-33-1D9B1E7C1748-A6816C12373A -->
### 2026-10-26 17:31:55 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 33

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/33`; SDKCONFIG SHA256-prefix `1D9B1E7C1748`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1442032 байт**; linker total_size: **1441672 байт**; статические data+bss: **91088 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1293886 | 0 | 0 |
| DIRAM | 217038 | 452112 | 235074 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### Измерение полной HTTP/MCP цепочки на C6 и корректировка task stack

Бенчмарк reader на main task8192 прошёл, но полная C6 MCP-демка при HTTP stack6144
перезагружалась. UART подтвердил Guru Meditation / Stack protection fault именно
в naw_tcp_http. Для yyjson faultPC addr2line: Arena::allocate ← yyjson_read_opts;
objdump показывает frame1440 B у yyjson_read_opts с GCC14 -Og. Отдельная bounded
проверка доходила до device_info и также вызвала stack protection fault в этом
же6144-байтовом task. Это не ошибка Wi-Fi и не просто таймаут Node-клиента.
Изначальная гипотеза о клиентском пути не объясняла эти конкретные C6 отказы.

Оба C6 MCP-профиля переведены на8192 B. Kconfig теперь даёт8192 при MCP и6144
без него; native HTTPD получил отдельный параметр NAWSERVER_HTTPD_STACK_SIZE с
такими же начальными значениями вместо постоянного6144. Сохранённые sdkconfig
не перетираются общей библиотекой. В локальных профилях30/33 значение выбрано явно.
HTTP-only C6 профиль остаётся6144. Дополнительные2048 B task reserve общие для
обеих MCP-конфигураций; относительная разница reader arenas по-прежнему14336 B.

С8192 C6/yyjson прошла прямой LAN MCP probe: discovery/list(4tools), device_info,
UTF-8echo, invalid-preserves-state, start/status/stop. Через INFO подтверждён
backendyyjson, arena16384/output2048/input4096, ELF
f381c16b88f54bcd42735828526928ad5bb8a76ae73fe9b1ce8493c7d7c11c09.
Task API после проверки: naw_tcp_http stack_min_free_bytes=1904. Сырые fault logs
сохранены ignored local/json26-c6-{yyjson,bounded}-stack6144.log; успешные API
снимки local/json26-c6-yyjson8k-*.cbor. Частичные и неудачные прогоны не считаютсяPASS.

### 26.10.2026 — итог выбора JSON и повторного использования yyjson

Исходники: 632dafd + текущие изменения. SDK/пакеты окружения не обновлялись. Первый внешний reader — yyjson 0.13.0 (MIT); общий интерфейс Document/Value скрывает DOM. Writer остаётся общим ограниченным encoder. Bounded оставлен начальным выбором. Define/Kconfig вынесены и прокомментированы; увеличенный профиль настроек проверяется отдельным набором тестов.

#### Короткий одинаковый корпус на физической C6

ESP32-C6, 160 МГц; ESP-IDF 5.5.6, GCC 14.2, -Og; main stack8192. Без Wi-Fi/LCD. Шесть документов, 200 итераций каждого, 1200 разборов; максимум корпуса 127 значений/1109 байт, глубина8, строка512. Это не максимальное потребление при всех разрешённых документах. Время включает reader и потребление результата; в reuse-паре также реальный независимый вызов yyjson приложением. Writer прогревается и проверяется вне таймера.

| Профиль | Reader | Приложение уже вызывает yyjson | BIN, Б | data+bss, Б | Арена reserve / наблюдаемый peak, Б | 1200 разборов, мкс |
|---|---|---|---:|---:|---:|---:|
| 36 | nawserver_bounded | нет | 285408 | 13416 | 2048 / 1016 | 334462 |
| 37 | yyjson | нет | 400528 | 27752 | 16384 / 4216 | 243340 |
| 38 | nawserver_bounded | да | 401296 | 13416 | 2048 / 1016 | 426415 |
| 39 | yyjson | да | 400688 | 27752 | 16384 / 4216 | 325047 |

Первое подключение yyjson: +115120 Б Flash и +14336 Б статической арены против bounded. На этом корпусе время без независимого app consumer меньше на 27,2%. Если yyjson уже реально используется приложением: повторное использование экономит 608 Б Flash, но арена всё равно больше на14336 Б. В reuse-паре время меньше на23,8%. Это результаты конкретного корпуса и опций, не общий рейтинг библиотек.

Наблюдаемый peak — конец наиболее далёкой выделенной области reader с префиксом/предшествующей служебной памятью пула. Он не включает весь стек, HTTP, выходной буфер2048 Б и полный writer validation peak. У reader нет перехода к heap. Независимый app consumer в reuse-паре использует штатный heap yyjson и освобождает документ; это отдельный расход, минимум heap упал на4164 Б и восстановился после прохода.

#### Полные образы демо

| Профиль | Плата / SDK | Reader | BIN, Б | data+bss, Б |
|---|---|---|---:|---:|
| 30 | C6 / 5.5.6 | bounded | 1325856 | 76752 |
| 33 | C6 / 5.5.6 | yyjson | 1442032 | 91088 |
| 29 | S3 / 5.5.6 | bounded | 1530288 | 74896 |
| 34 | S3 / 5.5.6 | yyjson | 1640912 | 74896 |
| 28 | C3 / 6.1.0 | bounded | 1264048 | 69216 |
| 35 | C3 / 6.1.0 | yyjson | 1361984 | 83552 |
| 31 | Minimal C3 / 5.5.6 | bounded | 991568 | 52644 |
| 32 | Minimal C3 / 6.1.0 | bounded | 1005680 | 54512 |

C6 полная демо: yyjson +116176 Б Flash / +14336 Б data+bss; HTTP task в обоих профилях8192 Б. S3: +110624 Б Flash, data+bss одинаковы; дополнительная арена14336 Б выделяется из PSRAM при запуске. Эти Flash-разницы относятся ко всему связанному MCP-коду демо и отличаются от изолированного reader fixture.

#### Итоговые проверки и восстановление

Оба backend: CTest4/4 (JSON/MCP160, demo33, изменённые настройки29 проверок для каждого reader), плюс8 отрицательных конфигураций на этапе CMake. Базовый MCP-off CTest46/46 после изменения настраиваемого native HTTPD stack. Frontend21/21, три TypeScript/Vite build и упаковка assets. Host native/TCP demo собираются с обоими выбранными reader. IDF5.5.6: C6/S3 bounded и yyjson, Minimal bounded; IDF6.1.0: C3 showcase bounded и yyjson, Minimal bounded — сборкиPASS.

Физические проверки: все четыре изолированных C6 benchmarkPASS; полная C6 yyjson с HTTP stack8192 — discovery/list4, device_info, UTF-8echo, invalid-preserves-state, start/status/stopPASS. Затем C6 восстановлена на профиль30 bounded; S3 на профиль29 bounded. Обе повторно прошли тот же короткий прямой LAN MCP probe. ELF каждого работающего образа совпал с локально собранным. Подробности ниже; TCP backend и JSON-бюджеты взяты из INFO устройства. Эксперименты после проверок выключены.

Текущие runtime heap snapshots сняты при подключённых demo-клиентах, поэтому не являются контролируемым сравнением reader. У S3 internal min_free7924 Б за текущую загрузку — небольшая история минимального остатка, требующая отдельной оценки при будущем нагрузочном тесте; измерение не объявляет проблему устранённой. S3/yyjson в этой фазе только собрана; native HTTPD MCP-on и Linux runtime не проверялись. Увеличение native HTTPD stack проверено базовыми тестами, но не физическим MCP-прогоном. Длительных нагрузок не запускалось; прежние отдельные408 не объявляются устранёнными.

Для минимальной MCP-демо ID возвращается типизированными string/int64/uint64 вызовами: generic Writer::value не затягивает форматирование double без надобности. Это убрало выявленное промежуточное увеличение Flash примерно на110 КиБ; link map Minimal не содержит живого Writer::real. Промежуточные сбои/замеры выше сохранены как история.

Точные SHA/config fingerprints, heap/stack и контрольные суммы финальных прогонов:

```json
{
  "measured_utc": "2026-10-26T12:40:48.813Z",
  "assets": [
    {
      "path": "examples/c6_lcd147/main/assets.hpp",
      "sha256": "0148b360202aa95f814fa92d32c953ccca31ddba8b2ea79eadcab7e7b6065408"
    },
    {
      "path": "examples/touch_lcd7/main/assets.hpp",
      "sha256": "a0df46cfa6a934c94e23b75039da4f06e9fc7a141c36acc7255d53317f57d76a"
    },
    {
      "path": "examples/showcase/main/assets.hpp",
      "sha256": "6f79479e7afba730d07161826230206f8fd7e4b267dd39bd39f88b5484983cbb"
    }
  ],
  "benchmarks": [
    {
      "profile": 36,
      "target": "esp32c6",
      "bin_bytes": 285408,
      "static_data_bss_bytes": 13416,
      "sdkconfig_sha256": "06f5222b2646c89dfcebb96b4b73b0308510a47944a3c9135aeaa7b36dbcae87",
      "bin_sha256": "86c5157a3421156887f45ccf687dc32cd4be96e80b321a7d9ec79a9f6fa7f9ff",
      "elf_sha256": "7040dc8f3f96946ab9de48c294d6aa8dc5d887d4a394c1ac455029fa876d9054",
      "measured_utc": "2026-10-26T12:06:03.3483073Z",
      "chip": "esp32c6",
      "mac": "DEVICE-MAC",
      "benchmark": {
        "backend": "nawserver_bounded",
        "ok": true,
        "reserved_arena_bytes": 2048,
        "reader_parse_peak_bytes": 1016,
        "maximum_value_count": 127,
        "output_reserved_bytes": 2048,
        "maximum_encoded_output_bytes": 1109,
        "fixtures": 6,
        "iterations": 200,
        "parses": 1200,
        "elapsed_us": 334462,
        "existing_application_yyjson": false,
        "checksum": 257600
      },
      "heap_and_stack": [
        "JSON_BENCH_HEAP before=463316 minimum=463316 stack_free=6908",
        "JSON_BENCH_DONE result=0 heap_after=463316 heap_minimum=463316 stack_free=6672"
      ]
    },
    {
      "profile": 37,
      "target": "esp32c6",
      "bin_bytes": 400528,
      "static_data_bss_bytes": 27752,
      "sdkconfig_sha256": "042112f0d0b022c6318b18d7c77a58e72ee3cc1fdf9d579efc53bc1dc4eb1212",
      "bin_sha256": "e4485e386e227ca318d25ac5405da533def2c036a0fe709172a836069ef5c019",
      "elf_sha256": "0a1b91dac29c1d2552d59a80144b1beb106af780454f1c5f0d09762c62dc5cd9",
      "measured_utc": "2026-10-26T12:06:21.6165806Z",
      "chip": "esp32c6",
      "mac": "DEVICE-MAC",
      "benchmark": {
        "backend": "yyjson",
        "ok": true,
        "reserved_arena_bytes": 16384,
        "reader_parse_peak_bytes": 4216,
        "maximum_value_count": 127,
        "output_reserved_bytes": 2048,
        "maximum_encoded_output_bytes": 1109,
        "fixtures": 6,
        "iterations": 200,
        "parses": 1200,
        "elapsed_us": 243340,
        "existing_application_yyjson": false,
        "checksum": 257600
      },
      "heap_and_stack": [
        "JSON_BENCH_HEAP before=448964 minimum=448964 stack_free=6908",
        "JSON_BENCH_DONE result=0 heap_after=448964 heap_minimum=448964 stack_free=6564"
      ]
    },
    {
      "profile": 38,
      "target": "esp32c6",
      "bin_bytes": 401296,
      "static_data_bss_bytes": 13416,
      "sdkconfig_sha256": "06f5222b2646c89dfcebb96b4b73b0308510a47944a3c9135aeaa7b36dbcae87",
      "bin_sha256": "93e0b1dee429d79990b239290ac1c58ff723a132158d6701e807b3f72b9e1d64",
      "elf_sha256": "019193f3065c8b10e43288f6329245554df0516699c8818024f610c21f17e849",
      "measured_utc": "2026-10-26T12:06:39.8933526Z",
      "chip": "esp32c6",
      "mac": "DEVICE-MAC",
      "benchmark": {
        "backend": "nawserver_bounded",
        "ok": true,
        "reserved_arena_bytes": 2048,
        "reader_parse_peak_bytes": 1016,
        "maximum_value_count": 127,
        "output_reserved_bytes": 2048,
        "maximum_encoded_output_bytes": 1109,
        "fixtures": 6,
        "iterations": 200,
        "parses": 1200,
        "elapsed_us": 426415,
        "existing_application_yyjson": true,
        "checksum": 723800
      },
      "heap_and_stack": [
        "JSON_BENCH_HEAP before=463300 minimum=463300 stack_free=6908",
        "JSON_BENCH_DONE result=0 heap_after=463300 heap_minimum=459136 stack_free=6528"
      ]
    },
    {
      "profile": 39,
      "target": "esp32c6",
      "bin_bytes": 400688,
      "static_data_bss_bytes": 27752,
      "sdkconfig_sha256": "042112f0d0b022c6318b18d7c77a58e72ee3cc1fdf9d579efc53bc1dc4eb1212",
      "bin_sha256": "a6f2003ac28addcefb94dc59bf7d77c9a470f776f7f04df24ae78a48e967542b",
      "elf_sha256": "acb8d1602b5beba80286a36e77283286491aed9be8f1f6dd861beca75e4f4a47",
      "measured_utc": "2026-10-26T12:06:58.0785670Z",
      "chip": "esp32c6",
      "mac": "DEVICE-MAC",
      "benchmark": {
        "backend": "yyjson",
        "ok": true,
        "reserved_arena_bytes": 16384,
        "reader_parse_peak_bytes": 4216,
        "maximum_value_count": 127,
        "output_reserved_bytes": 2048,
        "maximum_encoded_output_bytes": 1109,
        "fixtures": 6,
        "iterations": 200,
        "parses": 1200,
        "elapsed_us": 325047,
        "existing_application_yyjson": true,
        "checksum": 723800
      },
      "heap_and_stack": [
        "JSON_BENCH_HEAP before=448964 minimum=448964 stack_free=6908",
        "JSON_BENCH_DONE result=0 heap_after=448964 heap_minimum=444800 stack_free=6528"
      ]
    }
  ],
  "demos": [
    {
      "profile": 30,
      "target": "esp32c6",
      "bin_bytes": 1325856,
      "static_data_bss_bytes": 76752,
      "sdkconfig_sha256": "11c65f72180c7c094fa9d3adf995141293fb0cc1fbba5028ba63a4596d2c842c",
      "bin_sha256": "00cc72b6e770fa942f76967c821dc50cdb2f006433e9c59d8d7dcf336eb8b346",
      "elf_sha256": "10cd9ca2dbee2b34631671ae753d5590514556e9477867bbf8ba2fee343dfa97"
    },
    {
      "profile": 33,
      "target": "esp32c6",
      "bin_bytes": 1442032,
      "static_data_bss_bytes": 91088,
      "sdkconfig_sha256": "1d9b1e7c1748fb46c6e73b7483184d0a415d66f0493a4be28afdb64d590c67ab",
      "bin_sha256": "a6816c12373a851f4496b0ad68b07d057816ea4ed415606555ff1bd9e4682f99",
      "elf_sha256": "f381c16b88f54bcd42735828526928ad5bb8a76ae73fe9b1ce8493c7d7c11c09"
    },
    {
      "profile": 29,
      "target": "esp32s3",
      "bin_bytes": 1530288,
      "static_data_bss_bytes": 74896,
      "sdkconfig_sha256": "bd7604c3b4fce3be448c3ee5b1b26ddf6f75ba0274dad4a09deefc92dbd2420b",
      "bin_sha256": "4291b5829f7da21caf2d5cb8a7e01bd280a03c677cfc8bff38ad4d73a0372558",
      "elf_sha256": "f5a80721e29a13e3ec28426d8fe60c2e185a04b303af6035b2bbc5c7533eaec0"
    },
    {
      "profile": 34,
      "target": "esp32s3",
      "bin_bytes": 1640912,
      "static_data_bss_bytes": 74896,
      "sdkconfig_sha256": "2143fb55a31001a9253de76a3915fe96e128ffe89893d99985d17b26e65a6329",
      "bin_sha256": "6b6862aa18c414eb561b8adcfef9c730f0957e4e9644d4d9c14f751f28f3e4c4",
      "elf_sha256": "926322d7bfbd2674a887a43d76e9eca192b2e29d511725ffae1f26554a43d792"
    },
    {
      "profile": 28,
      "target": "esp32c3",
      "bin_bytes": 1264048,
      "static_data_bss_bytes": 69216,
      "sdkconfig_sha256": "b63b22487d6b655803d25ea521797065f606124752a37b542334611cff1e5173",
      "bin_sha256": "56a6085d2b9e5d4804a97dd8059b3d013afa8f8d25ad1b2a7d5e91c0c2464cf6",
      "elf_sha256": "80fe955ecc31bf982ffb6ebb48ae4694bf710546ced69b83983278790f26b409"
    },
    {
      "profile": 35,
      "target": "esp32c3",
      "bin_bytes": 1361984,
      "static_data_bss_bytes": 83552,
      "sdkconfig_sha256": "69656bf5e81bd6fa2aa795aa9bd6503e7c54d27ff139c110be6926606b9f84f1",
      "bin_sha256": "1f3b38cc17c24e0d4edf18f290ff8a33f6ebb2b212e644b0dfdc9c01f34d7bfd",
      "elf_sha256": "1a056db641770ab7f2003505f03d80cf04cf40edbfe64101f8ea02cbf716efa4"
    },
    {
      "profile": 31,
      "target": "esp32c3",
      "bin_bytes": 991568,
      "static_data_bss_bytes": 52644,
      "sdkconfig_sha256": "483437576a824af71fcf11ff840d9235c50449aa668bf38aca5e72b99cc3c6a7",
      "bin_sha256": "055bee9b9090f070c2a4b9f1d00938d5e9dcb7b27dd95fbd9a7380a9225c7a2d",
      "elf_sha256": "f9113aa7be64f08738aa855b134aafe051d85931dba87f2874a09406b84380f5"
    },
    {
      "profile": 32,
      "target": "esp32c3",
      "bin_bytes": 1005680,
      "static_data_bss_bytes": 54512,
      "sdkconfig_sha256": "d0b4e776ee5ba82882a91012a9bf54f4622bf6c13c9c4cc0b94d4e73255658ab",
      "bin_sha256": "7a32736f4978d64f9174e9bc644f4d2912e7100ce2e4c86d7d36865d2d03673a",
      "elf_sha256": "8de7179172cc5921745063f2dc7ee7a84edcc21aafafbb55fc64f084e8f32a34"
    }
  ],
  "runtime": [
    {
      "board": "c6",
      "profile": 30,
      "measured_utc": "2026-10-26T12:40:48.811Z",
      "elf_sha256": "10cd9ca2dbee2b34631671ae753d5590514556e9477867bbf8ba2fee343dfa97",
      "boot_id": "c6-0fc0dc4b-1f592077",
      "ip": "192.0.2.1",
      "mac": "DEVICE-MAC",
      "firmware": {
        "name": "NaWServer_c6_lcd147",
        "version": "0.1.0-c6-demo",
        "idf_version": "5.5.6",
        "json_backend": "nawserver_bounded",
        "json_workspace_bytes": 2048,
        "mcp_output_bytes": 2048,
        "mcp_input_bytes": 4096,
        "mcp_enabled": true,
        "http_backend": "tcp",
        "board_label": "Waveshare C6 LCD 1.47",
        "build_date": "Oct 26 2026",
        "build_time": "17:26:04",
        "reset_reason": 11
      },
      "uptime_ms": 449282,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 335196,
          "free_bytes": 189812,
          "min_free_bytes": 172372,
          "largest_block_bytes": 167936
        }
      ],
      "ws_clients": 1,
      "experiments_running": false,
      "http_task": {
        "id": "task-11",
        "name": "naw_tcp_http",
        "state": "blocked",
        "priority": 5,
        "core_affinity": 0,
        "stack_min_free_bytes": 1948,
        "runtime_ticks": 5498205,
        "cpu_pct": 0.618196290822255
      },
      "mcp_probe": {
        "status": "PASS",
        "tools": 4,
        "utf8_echo": true,
        "invalid_preserves_state": true,
        "start_status_stop": true
      }
    },
    {
      "board": "s3",
      "profile": 29,
      "measured_utc": "2026-10-26T12:40:48.813Z",
      "elf_sha256": "f5a80721e29a13e3ec28426d8fe60c2e185a04b303af6035b2bbc5c7533eaec0",
      "boot_id": "s3-58b88da3-f719470f",
      "ip": "192.0.2.2",
      "mac": "DEVICE-MAC",
      "firmware": {
        "name": "NaWServer_touch_lcd7",
        "version": "0.1.0-touch-lcd7",
        "idf_version": "5.5.6",
        "json_backend": "nawserver_bounded",
        "json_workspace_bytes": 2048,
        "mcp_output_bytes": 2048,
        "mcp_input_bytes": 4096,
        "mcp_enabled": true,
        "http_backend": "tcp",
        "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
        "build_date": "Oct 26 2026",
        "build_time": "16:44:32",
        "reset_reason": 1
      },
      "uptime_ms": 624764,
      "memory": [
        {
          "id": "internal",
          "label": "Внутренняя RAM",
          "total_bytes": 284043,
          "free_bytes": 42267,
          "min_free_bytes": 7924,
          "largest_block_bytes": 29696
        },
        {
          "id": "psram",
          "label": "PSRAM",
          "total_bytes": 7004544,
          "free_bytes": 5460332,
          "min_free_bytes": 5460332,
          "largest_block_bytes": 5373952
        }
      ],
      "ws_clients": 1,
      "experiments_running": false,
      "http_task": {
        "id": "task-15",
        "name": "naw_tcp_http",
        "state": "blocked",
        "priority": 5,
        "core_affinity": null,
        "stack_min_free_bytes": 1412,
        "runtime_ticks": 9201036,
        "cpu_pct": 0.4392995607004393
      },
      "mcp_probe": {
        "status": "PASS",
        "tools": 4,
        "utf8_echo": true,
        "invalid_preserves_state": true,
        "start_status_stop": true
      }
    }
  ]
}
```

### JSON benchmark C6, профиль 43

```json
{
  "profile": 43,
  "measured_utc": "2026-10-26T13:06:23.2271457Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "rapidjson",
    "ok": true,
    "reserved_arena_bytes": 8192,
    "reader_parse_peak_bytes": 2126,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 347419,
    "existing_application_yyjson": false,
    "checksum": 257600
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=457172 minimum=457172 stack_free=6908",
    "JSON_BENCH_DONE result=0 heap_after=457172 heap_minimum=457172 stack_free=6416"
  ]
}
```

### JSON benchmark C6, профиль 44

```json
{
  "profile": 44,
  "measured_utc": "2026-10-26T13:06:41.2281223Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "nawserver_bounded",
    "ok": true,
    "reserved_arena_bytes": 2048,
    "reader_parse_peak_bytes": 1016,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 877141,
    "existing_application_rapidjson": true,
    "checksum": 4239406539350723136
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=463300 minimum=463300 stack_free=6908",
    "JSON_BENCH_DONE result=0 heap_after=463300 heap_minimum=396036 stack_free=6624"
  ]
}
```

### JSON benchmark C6, профиль 45

```json
{
  "profile": 45,
  "measured_utc": "2026-10-26T13:06:59.4371514Z",
  "chip": "esp32c6",
  "mac": "DEVICE-MAC",
  "benchmark": {
    "backend": "rapidjson",
    "ok": true,
    "reserved_arena_bytes": 8192,
    "reader_parse_peak_bytes": 2126,
    "maximum_value_count": 127,
    "output_reserved_bytes": 2048,
    "maximum_encoded_output_bytes": 1109,
    "fixtures": 6,
    "iterations": 200,
    "parses": 1200,
    "elapsed_us": 899038,
    "existing_application_rapidjson": true,
    "checksum": 4239406539350723136
  },
  "heap_and_stack": [
    "JSON_BENCH_HEAP before=457156 minimum=457156 stack_free=6908",
    "JSON_BENCH_DONE result=0 heap_after=457156 heap_minimum=389892 stack_free=6416"
  ]
}
```
<!-- backend26-29-FBEDB1456C2A-682C2AE56DCD -->
### 2026-10-26 18:09:16 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 29

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/29`; SDKCONFIG SHA256-prefix `FBEDB1456C2A`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1531504 байт**; linker total_size: **1531393 байт**; статические data+bss: **74912 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 917034 | 0 | 0 |
| Flash Data | 466984 | 0 | 0 |
| DIRAM | 178375 | 341760 | 163385 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-34-5037605268BC-0F6AF7AD0A43 -->
### 2026-10-26 18:09:16 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 34

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/34`; SDKCONFIG SHA256-prefix `5037605268BC`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1642128 байт**; linker total_size: **1642013 байт**; статические data+bss: **74912 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1014678 | 0 | 0 |
| Flash Data | 479960 | 0 | 0 |
| DIRAM | 178375 | 341760 | 163385 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-40-554AB31EE2C1-21F38F469BA2 -->
### 2026-10-26 18:09:16 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 40

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/40`; SDKCONFIG SHA256-prefix `554AB31EE2C1`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_RAPIDJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=8192
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1336672 байт**; linker total_size: **1336312 байт**; статические data+bss: **82896 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1188526 | 0 | 0 |
| DIRAM | 208846 | 452112 | 243266 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-41-7A0A30747962-CCEF9A40573F -->
### 2026-10-26 18:09:17 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 41

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/41`; SDKCONFIG SHA256-prefix `7A0A30747962`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_RAPIDJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=8192
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1541392 байт**; linker total_size: **1541269 байт**; статические data+bss: **74912 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 921598 | 0 | 0 |
| Flash Data | 472296 | 0 | 0 |
| DIRAM | 178375 | 341760 | 163385 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-42-8ECA2086D42F-06CB190F16CB -->
### 2026-10-26 18:09:17 +05:00 — NaWServer_showcase, esp32c3, SDK 6.1.0, профиль 42

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/42`; SDKCONFIG SHA256-prefix `8ECA2086D42F`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_RAPIDJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=8192
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1274912 байт**; linker total_size: **1274544 байт**; статические data+bss: **75360 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 797148 | 0 | 0 |
| Flash Data | 374596 | 0 | 0 |
| DRAM | 160548 | 321296 | 160748 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-43-68BEA1907048-F1A9FC58AB3B -->
### 2026-10-26 18:09:17 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 43

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/43`; SDKCONFIG SHA256-prefix `68BEA1907048`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_RAPIDJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=8192
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **295296 байт**; linker total_size: **294974 байт**; статические data+bss: **19560 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 247112 | 0 | 0 |
| DIRAM | 62278 | 452112 | 389834 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-44-845E881C7F66-3D00F43EB633 -->
### 2026-10-26 18:09:18 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 44

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/44`; SDKCONFIG SHA256-prefix `845E881C7F66`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **311552 байт**; linker total_size: **311218 байт**; статические data+bss: **13416 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 263342 | 0 | 0 |
| DIRAM | 56148 | 452112 | 395964 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-45-68BEA1907048-489B245B6269 -->
### 2026-10-26 18:09:18 +05:00 — NaWServer_json_bench, esp32c6, SDK 5.5.6, профиль 45

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/45`; SDKCONFIG SHA256-prefix `68BEA1907048`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_RAPIDJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=8192
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **321664 байт**; linker total_size: **321326 байт**; статические data+bss: **19560 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 273450 | 0 | 0 |
| DIRAM | 62292 | 452112 | 389820 |
| LP SRAM | 24 | 16384 | 16360 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-46-15A30092EC86-20C759445D65 -->
### 2026-10-26 18:17:21 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 46

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/46`; SDKCONFIG SHA256-prefix `15A30092EC86`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1348336 байт**; linker total_size: **1347976 байт**; статические data+bss: **76720 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1199976 | 0 | 0 |
| DIRAM | 202892 | 452112 | 249220 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-47-CD2CD3E77F02-36CEFEDB1C09 -->
### 2026-10-26 18:17:22 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 47

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/47`; SDKCONFIG SHA256-prefix `CD2CD3E77F02`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1464528 байт**; linker total_size: **1464156 байт**; статические data+bss: **91056 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1316156 | 0 | 0 |
| DIRAM | 217228 | 452112 | 234884 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-48-26FF4E7D9E7A-5806A11C89A2 -->
### 2026-10-26 18:17:22 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 48

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/48`; SDKCONFIG SHA256-prefix `26FF4E7D9E7A`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_RAPIDJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=8192
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1359168 байт**; linker total_size: **1358796 байт**; статические data+bss: **82864 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1210796 | 0 | 0 |
| DIRAM | 209036 | 452112 | 243076 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-49-42571AC30B79-42CAA394DD31 -->
### 2026-10-26 18:17:23 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 49

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/49`; SDKCONFIG SHA256-prefix `42571AC30B79`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1551872 байт**; linker total_size: **1551757 байт**; статические data+bss: **74896 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 927118 | 0 | 0 |
| Flash Data | 477128 | 0 | 0 |
| DIRAM | 178503 | 341760 | 163257 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-50-1F26B0A2FDAB-D8DC007088E4 -->
### 2026-10-26 18:23:04 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 50

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/50`; SDKCONFIG SHA256-prefix `1F26B0A2FDAB`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1662688 байт**; linker total_size: **1662569 байт**; статические data+bss: **74896 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1024954 | 0 | 0 |
| Flash Data | 490104 | 0 | 0 |
| DIRAM | 178503 | 341760 | 163257 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-51-9A5EA0F29C3E-C876FA9818DE -->
### 2026-10-26 18:23:05 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 51

Исходники: `632dafd` + текущие изменения WS/TCP, optional MCP/JSON и финальные demo assets (26.10.2026); build `local/backend26-idf/51`; SDKCONFIG SHA256-prefix `9A5EA0F29C3E`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_RAPIDJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=8192
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1561728 байт**; linker total_size: **1561605 байт**; статические data+bss: **74896 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 931654 | 0 | 0 |
| Flash Data | 482440 | 0 | 0 |
| DIRAM | 178503 | 341760 | 163257 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### Ночные RapidJSON/MCP physical smoke и S3 Wi-Fi policy, 26.10.2026

```json
[
  {
    "board": "c6",
    "profile": 40,
    "measured_utc": "2026-10-26T13:08:53.099Z",
    "elf_sha256": "a47be69686d7a5125657cc5875d770afe2e5b2f44f038d84c3e4685884ed22c8",
    "boot_id": "c6-8450b284-893a3fdb",
    "firmware": {
      "name": "NaWServer_c6_lcd147",
      "version": "0.1.0-c6-demo",
      "idf_version": "5.5.6",
      "json_backend": "rapidjson",
      "json_workspace_bytes": 8192,
      "mcp_output_bytes": 2048,
      "mcp_input_bytes": 4096,
      "mcp_enabled": true,
      "http_backend": "tcp",
      "board_label": "Waveshare C6 LCD 1.47",
      "build_date": "Oct 26 2026",
      "build_time": "17:51:45",
      "reset_reason": 11
    },
    "memory": [
      {
        "id": "internal",
        "label": "Внутренняя RAM",
        "total_bytes": 329052,
        "free_bytes": 182032,
        "min_free_bytes": 168208,
        "largest_block_bytes": 159744
      }
    ],
    "http_task": {
      "id": "task-11",
      "name": "naw_tcp_http",
      "state": "blocked",
      "priority": 5,
      "core_affinity": 0,
      "stack_min_free_bytes": 1948,
      "runtime_ticks": 1073341,
      "cpu_pct": 5.933742409817835
    }
  },
  {
    "board": "s3",
    "profile": 41,
    "measured_utc": "2026-10-26T13:18:19.476Z",
    "elf_sha256": "5603534087a263bc6acad185695c231c2e1eded649a02e96d79aece0ccb1d671",
    "boot_id": "s3-b55c26b3-39e00170",
    "firmware": {
      "name": "NaWServer_touch_lcd7",
      "version": "0.1.0-touch-lcd7",
      "idf_version": "5.5.6",
      "json_backend": "rapidjson",
      "json_workspace_bytes": 8192,
      "mcp_output_bytes": 2048,
      "mcp_input_bytes": 4096,
      "mcp_enabled": true,
      "http_backend": "tcp",
      "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
      "build_date": "Oct 26 2026",
      "build_time": "17:53:49",
      "reset_reason": 1
    },
    "memory": [
      {
        "id": "internal",
        "label": "Внутренняя RAM",
        "total_bytes": 283771,
        "free_bytes": 41091,
        "min_free_bytes": 7872,
        "largest_block_bytes": 31744
      },
      {
        "id": "psram",
        "label": "PSRAM",
        "total_bytes": 6933248,
        "free_bytes": 5382892,
        "min_free_bytes": 5382892,
        "largest_block_bytes": 5242880
      }
    ],
    "http_task": {
      "id": "task-15",
      "name": "naw_tcp_http",
      "state": "blocked",
      "priority": 5,
      "core_affinity": null,
      "stack_min_free_bytes": 1556,
      "runtime_ticks": 5997969,
      "cpu_pct": 2.4843283141954333
    }
  },
  {
    "board": "s3",
    "profile": 29,
    "measured_utc": "2026-10-26T13:08:53.03Z",
    "elf_sha256": "9c98ac34fa124970154bf87a498550a9cfea1f9a1d59cfb443724284963b818b",
    "boot_id": "s3-ba0e2ec2-02cfc0f6",
    "firmware": {
      "name": "NaWServer_touch_lcd7",
      "version": "0.1.0-touch-lcd7",
      "idf_version": "5.5.6",
      "json_backend": "nawserver_bounded",
      "json_workspace_bytes": 2048,
      "mcp_output_bytes": 2048,
      "mcp_input_bytes": 4096,
      "mcp_enabled": true,
      "http_backend": "tcp",
      "board_label": "Waveshare ESP32-S3-Touch-LCD-7",
      "build_date": "Oct 26 2026",
      "build_time": "16:44:32",
      "reset_reason": 1
    },
    "memory": [
      {
        "id": "internal",
        "label": "Внутренняя RAM",
        "total_bytes": 283771,
        "free_bytes": 41407,
        "min_free_bytes": 7936,
        "largest_block_bytes": 31744
      },
      {
        "id": "psram",
        "label": "PSRAM",
        "total_bytes": 7004096,
        "free_bytes": 5459884,
        "min_free_bytes": 5459884,
        "largest_block_bytes": 5373952
      }
    ],
    "http_task": {
      "id": "task-15",
      "name": "naw_tcp_http",
      "state": "blocked",
      "priority": 5,
      "core_affinity": null,
      "stack_min_free_bytes": 1412,
      "runtime_ticks": 1592762,
      "cpu_pct": 2.00836405854841
    }
  }
]
```

### S3 four-hour cached observation final, 26.10.2026

```json
{
  "board": "s3",
  "profile": 29,
  "duration_ms": 14400152,
  "http_requests": 247068,
  "ws_sent": 108910,
  "ws_received": 108910,
  "sse_events": 28241,
  "errors": 1354,
  "timeouts": 1346,
  "boot_id": "s3-def2b0f8-2336b160",
  "integrity_failures": 0,
  "uart_panic": 0,
  "recovery_http": 200,
  "recovery_info": 200,
  "internal_historical_min_free": 96,
  "internal_sampled_free_min": 6663,
  "internal_sampled_free_max": 45451,
  "internal_sampled_min_largest": 1536,
  "internal_post_load_free": 46003,
  "internal_post_load_largest": 27648,
  "psram_post_load_free": 5459884,
  "strict_gate": "failed, preserved",
  "firmware": "TCP + nawserver_bounded",
  "experiment_running": false,
  "helpers_stopped": true
}
```

### C6 four-hour cached observation final, 26.10.2026; final cleanup

```json
{
  "board": "c6",
  "profile": 30,
  "mode": "cached observation",
  "duration_ms": 14400203,
  "http_requests": 235407,
  "verified_completed_ws_echo": 78888,
  "sse_events": 28798,
  "errors": 2139,
  "timeouts": 2134,
  "boot_id": "c6-c4e2c8cd-4c9033dd",
  "recovery_health": 200,
  "recovery_info": 200,
  "internal_sampled_min_free": 152568,
  "internal_sampled_max_free": 183248,
  "internal_historical_min_free": 108960,
  "internal_post_load_free": 184928,
  "internal_post_load_largest": 151552,
  "http_stack_minimum": 1936,
  "strict_gate": "failed, preserved",
  "helpers_stopped": true,
  "final_firmware": "TCP + nawserver_bounded + MCP",
  "experiment_running": false,
  "counter_caveat": "WS sent/received count only completed verified exchanges; not all attempts"
}
```

Итог ночи: [readme_json_night_results_ru.md](readme_json_night_results_ru.md). Все управляемые runner/load/serial/sampler/queue завершены; оба board-lock освобождены, MAC/ELF текущих TCP+bounded прошивок совпали. SDK не обновлялись, commit/push не выполнялись.
<!-- backend26-52-A2DB44A9A4FD-466BAF2D03BF -->
### 2026-10-27 09:31:45 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 52

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/52`; SDKCONFIG SHA256-prefix `A2DB44A9A4FD`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1533936 байт**; linker total_size: **1533817 байт**; статические data+bss: **62696 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 919522 | 0 | 0 |
| Flash Data | 466920 | 0 | 0 |
| DIRAM | 166159 | 341760 | 175601 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-53-C909F2769AB1-2A6E36D2E9A5 -->
### 2026-10-27 09:31:45 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 53

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/53`; SDKCONFIG SHA256-prefix `C909F2769AB1`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1328272 байт**; linker total_size: **1327918 байт**; статические data+bss: **64528 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1180132 | 0 | 0 |
| DIRAM | 190478 | 452112 | 261634 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-54-783115513127-513F3ACA5025 -->
### 2026-10-27 09:31:46 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 54

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/54`; SDKCONFIG SHA256-prefix `783115513127`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=2
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=2
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1534240 байт**; linker total_size: **1534125 байт**; статические data+bss: **62728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 919798 | 0 | 0 |
| Flash Data | 466920 | 0 | 0 |
| DIRAM | 166191 | 341760 | 175569 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-55-554F8383E9D7-4AA5EBEB5BCA -->
### 2026-10-27 09:31:46 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 55

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/55`; SDKCONFIG SHA256-prefix `554F8383E9D7`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **970464 байт**; linker total_size: **970094 байт**; статические data+bss: **38112 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 730880 | 0 | 0 |
| Flash Data | 153212 | 0 | 0 |
| DRAM | 110958 | 321296 | 210338 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-56-80EDC8E2BEBF-EF7D081A4D1D -->
### 2026-10-27 09:31:46 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 56

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/56`; SDKCONFIG SHA256-prefix `80EDC8E2BEBF`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1552864 байт**; linker total_size: **1552749 байт**; статические data+bss: **62632 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 928142 | 0 | 0 |
| Flash Data | 477096 | 0 | 0 |
| DIRAM | 166239 | 341760 | 175521 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-52-F3E154E253B7-E120E5ED0BF9 -->
### 2026-10-27 09:53:47 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 52

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/52`; SDKCONFIG SHA256-prefix `F3E154E253B7`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1534208 байт**; linker total_size: **1534093 байт**; статические data+bss: **62728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 919782 | 0 | 0 |
| Flash Data | 466904 | 0 | 0 |
| DIRAM | 166191 | 341760 | 175569 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-53-C909F2769AB1-888C772A5020 -->
### 2026-10-27 09:53:47 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 53

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/53`; SDKCONFIG SHA256-prefix `C909F2769AB1`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1328720 байт**; linker total_size: **1328356 байт**; статические data+bss: **64552 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1180434 | 0 | 0 |
| DIRAM | 190614 | 452112 | 261498 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-54-783115513127-8268D000B5DA -->
### 2026-10-27 09:53:47 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 54

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/54`; SDKCONFIG SHA256-prefix `783115513127`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=2
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=2
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1534224 байт**; linker total_size: **1534113 байт**; статические data+bss: **62728 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 919802 | 0 | 0 |
| Flash Data | 466904 | 0 | 0 |
| DIRAM | 166191 | 341760 | 175569 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-55-554F8383E9D7-855F49B1F17C -->
### 2026-10-27 09:53:48 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 55

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/55`; SDKCONFIG SHA256-prefix `554F8383E9D7`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **970704 байт**; linker total_size: **970332 байт**; статические data+bss: **38136 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 731094 | 0 | 0 |
| Flash Data | 153212 | 0 | 0 |
| DRAM | 110982 | 321296 | 210314 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-56-80EDC8E2BEBF-EF4FE7BC5600 -->
### 2026-10-27 09:53:48 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 56

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/56`; SDKCONFIG SHA256-prefix `80EDC8E2BEBF`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1553136 байт**; linker total_size: **1553017 байт**; статические data+bss: **62664 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 928378 | 0 | 0 |
| Flash Data | 477096 | 0 | 0 |
| DIRAM | 166271 | 341760 | 175489 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-56-80EDC8E2BEBF-938C59F57D5A -->
### 2026-10-27 10:01:48 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 56

Исходники: `632dafd` + настройки памяти TCP/SSE, жёсткие лимиты и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/56`; SDKCONFIG SHA256-prefix `80EDC8E2BEBF`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1553136 байт**; linker total_size: **1553025 байт**; статические data+bss: **62664 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 928386 | 0 | 0 |
| Flash Data | 477096 | 0 | 0 |
| DIRAM | 166271 | 341760 | 175489 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### 2026-10-27 — завершение проверки настроек памяти и лимитов

Контракт и результаты: [датированный отчёт](readme_20261027_memory_limits_tests_ru.md).

| Профиль | Конфигурация | Final internal free / largest, Б | HTTP stack watermark, Б |
|---|---|---:|---:|
| 53 / C6 | TCP + bounded + MCP; System | 188840 / 163840 | 1912 |
| 54 / S3 | TCP + bounded + MCP; TCP/SSE PSRAM-only | 79071 / 34816 | 1412 |

ELF и MAC совпали с прошивками после fresh identity/range gate и normal vendor verify.
Нет наших активных load/observer/serial процессов; эксперименты выключены.
Обычная REST-приёмка не пройдена: редкие dial timeout сохранены отдельно от перегрузки.
SDK min_free_bytes — сумма региональных исторических минимумов, не одновременный
остаток всей RAM. Пиковые краткие провалы могли быть пропущены sampler10 секунд.
Окружение не обновлялось, commit/push не выполнялись.
<!-- backend26-53-C909F2769AB1-AC8E09D05EE3 -->
### 2026-10-27 11:35:14 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 53

Исходники: `632dafd` + настройки памяти, TCP_NODELAY и bounded close HTTP/WS (27.10.2026); build `local/backend26-idf/53`; SDKCONFIG SHA256-prefix `C909F2769AB1`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1328800 байт**; linker total_size: **1328446 байт**; статические data+bss: **64568 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1180524 | 0 | 0 |
| DIRAM | 190630 | 452112 | 261482 |
| LP SRAM | 56 | 16384 | 16328 |

Статический результат сборки; runtime C6/S3 и точные ELF записаны отдельно в docs/readme_20261027_tcp_rest_ws_close_ru.md. Профиль55 проверен только сборкой.

<!-- backend26-54-783115513127-04D146B9AE13 -->
### 2026-10-27 11:35:15 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 54

Исходники: `632dafd` + настройки памяти, TCP_NODELAY и bounded close HTTP/WS (27.10.2026); build `local/backend26-idf/54`; SDKCONFIG SHA256-prefix `783115513127`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=2
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=2
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1534416 байт**; linker total_size: **1534297 байт**; статические data+bss: **62736 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 919986 | 0 | 0 |
| Flash Data | 466904 | 0 | 0 |
| DIRAM | 166199 | 341760 | 175561 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Статический результат сборки; runtime C6/S3 и точные ELF записаны отдельно в docs/readme_20261027_tcp_rest_ws_close_ru.md. Профиль55 проверен только сборкой.

<!-- backend26-55-554F8383E9D7-B35C07EA427A -->
### 2026-10-27 11:35:15 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 55

Исходники: `632dafd` + настройки памяти, TCP_NODELAY и bounded close HTTP/WS (27.10.2026); build `local/backend26-idf/55`; SDKCONFIG SHA256-prefix `554F8383E9D7`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **970832 байт**; linker total_size: **970464 байт**; статические data+bss: **38144 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 731226 | 0 | 0 |
| Flash Data | 153212 | 0 | 0 |
| DRAM | 110990 | 321296 | 210306 |
| RTC SLOW | 60 | 8192 | 8132 |

Статический результат сборки; runtime C6/S3 и точные ELF записаны отдельно в docs/readme_20261027_tcp_rest_ws_close_ru.md. Профиль55 проверен только сборкой.


## 27.10.2026 — ограниченное закрытие TCP, обычные REST и WS

Исправление и полный датированный срез: [REST/WS C6/S3](readme_20261027_tcp_rest_ws_close_ru.md).
Профили53/54: SDK5.5.6, TCP+bounded, MCP; C6 System/System, S3 PsramOnly/PsramOnly.
Профиль55: SDK6.1.0 C3 TCP, только сборка. SDK/пакеты не обновлялись.

```ini
TCP_NODELAY=1
TcpOptions.close_timeout_ms=250
TcpOptions.close_drain_bytes=8192
TcpOptions.buffer_memory.max_bytes=65536
sse.buffer_memory.max_bytes=16384
WS.max_message_size=4096
WS.max_pending_bytes=16384
TCP.WS_TX=4x4096
```

| Плата | Обычный REST 1/2/3, две серии60 с | Min sampled internal / largest, Б | PSRAM min sampled, Б | WS Close+EOF | Перегрузка30 с |
|---|---|---:|---:|---|---|
| C6 | 720 HTTP200;0 errors/timeouts | 189816 / 151552 | — | 10/10; max EOF295.2 мс | 260HTTP/97WS/59SSE;3 timeout; recovery200 |
| S3 | 719 HTTP200;0 errors/timeouts | 79991 / 31744 | 5350772 | 10/10; max EOF649.0 мс | 108HTTP/40WS/53SSE;5 timeout; recovery200 |

Overload RAM отдельно: C6 sampled min169816/largest116736; S3 61183/largest19456,
PSRAM5341396 Б. Исторический SDK minsum S3 8324 Б не означает одновременный free.
После cleanup: C6 free191708 Б, S3 free86423/PSRAM5350772 Б. HTTP stack min2528/1756 Б.
Вторую серию выполняли после WS/negative/overload, без reset; boot stable.
Первая серия примерно06:27–06:31UTC, повтор06:36–06:39UTC (местное время+05).
0 exit stress runner означает cleanup, а не строгий PASS при ненулевых errors.
Windows48/48, Linux6/6 затронутых тестов, Go test/vet PASS; C3 SDK6 build PASS.
Ошибочный первый SDK6 wrapper со старым esptool исправлен выбором уже установленного
vendor tool, без обновления. Raw evidence и source manifest: ignored local/rest27.
<!-- backend26-53-B44DFC83DDB1-A26821152959 -->
### 2026-10-27 13:14:46 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 53

Исходники: `632dafd` + опциональный admission/QoS, резерв клиента по ключу и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/53`; SDKCONFIG SHA256-prefix `B44DFC83DDB1`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_ADMISSION=y
CONFIG_NAWSERVER_TCP_ADMISSION_HEAD_BYTES=1024
CONFIG_NAWSERVER_TCP_ADMISSION_RESPONSE_BYTES=4096
CONFIG_NAWSERVER_TCP_ADMISSION_STREAM_BYTES=12288
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=131072
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1342304 байт**; linker total_size: **1341938 байт**; статические data+bss: **66136 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1194016 | 0 | 0 |
| DIRAM | 192198 | 452112 | 259914 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-54-D7B211F22102-C7C1042C18DC -->
### 2026-10-27 13:14:46 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 54

Исходники: `632dafd` + опциональный admission/QoS, резерв клиента по ключу и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/54`; SDKCONFIG SHA256-prefix `D7B211F22102`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_ADMISSION=y
CONFIG_NAWSERVER_TCP_ADMISSION_HEAD_BYTES=1024
CONFIG_NAWSERVER_TCP_ADMISSION_RESPONSE_BYTES=4096
CONFIG_NAWSERVER_TCP_ADMISSION_STREAM_BYTES=12288
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=2
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=131072
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=2
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1546000 байт**; linker total_size: **1545881 байт**; статические data+bss: **63464 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 930690 | 0 | 0 |
| Flash Data | 467784 | 0 | 0 |
| DIRAM | 166927 | 341760 | 174833 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-55-3333963DA019-18C2D79B0687 -->
### 2026-10-27 13:14:46 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 55

Исходники: `632dafd` + опциональный admission/QoS, резерв клиента по ключу и текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/55`; SDKCONFIG SHA256-prefix `3333963DA019`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_TCP_ADMISSION is not set
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **973552 байт**; linker total_size: **973180 байт**; статические data+bss: **38208 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 733854 | 0 | 0 |
| Flash Data | 153300 | 0 | 0 |
| DRAM | 111054 | 321296 | 210242 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### 2026-10-27 13:20 +05:00 — runtime admission/QoS C6/S3, SDK5.5.6

Полная конфигурация, SHA и ограничения: [датированный отчёт](readme_20261027_admission_qos_ru.md).
TCP+bounded, admission ON; TCP/SSE byte budgets131072/16384 Б, primary HTTP1,
shared2×(HTTP4/WS1/SSE1), ingress4, registry14, sockets24. C6 System, S3 PsramOnly.

| Профиль | C6 | S3 |
|---|---|---|
| Обычные REST1/2/3, по60 с |60/120/180 HTTP200;0 ошибок|60/120/180 HTTP200;0 ошибок|
| Min sampled internal обычные1/2/3, Б |90576 /91816 /91488|86419 /84107 /83947|
| Перегрузка60 с, primary2/s +keeper1/s +mixed2×50ms |Primary120/120;598×503;1088×429;0 timeout|Primary120/120;578×503;1031×429;0 timeout|
| Min sampled internal на перегрузке, Б |84652|77207|
| Max полного503 / max guest-rate ответа, мс |92.7 /102.2|144.8 /144.1|
| Длительные WS/SSE |10+10 лишних503;10/10 pong;SSE data есть|10+10 лишних503;10/10 pong;SSE data есть|
| После прогонов internal / PSRAM free, Б |93776 /—|83331 /5247160|

Boot stable, recovery200, experiment.running=false. Каждая отдельная нагрузка
менее2 минут. Min sampled — отсчёты текущего free, не SDK per-region historical sum.
Host52/52, Linux7/7, host admission OFF3/3. SDK6 C3 admission OFF — build only.
Прежний агрессивный reload stress не повторялся и не объявлен исправленным.
Полное заполнение ingress — TCP close, отдельно от HTTP503/429. Окружение не обновлялось.
<!-- backend26-53-B44DFC83DDB1-65046882B0F4 -->
### 2026-10-27 16:07:10 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 53

Исходники: `632dafd` + managed memory initial/dynamic + API reserve + keep-alive, текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/53`; SDKCONFIG SHA256-prefix `B44DFC83DDB1`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_ADMISSION=y
CONFIG_NAWSERVER_TCP_ADMISSION_HEAD_BYTES=1024
CONFIG_NAWSERVER_TCP_ADMISSION_RESPONSE_BYTES=4096
CONFIG_NAWSERVER_TCP_ADMISSION_STREAM_BYTES=12288
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=131072
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1350544 байт**; linker total_size: **1350178 байт**; статические data+bss: **66312 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1202256 | 0 | 0 |
| DIRAM | 192374 | 452112 | 259738 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-54-D7B211F22102-B01A713B1629 -->
### 2026-10-27 16:07:10 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 54

Исходники: `632dafd` + managed memory initial/dynamic + API reserve + keep-alive, текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/54`; SDKCONFIG SHA256-prefix `D7B211F22102`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_ADMISSION=y
CONFIG_NAWSERVER_TCP_ADMISSION_HEAD_BYTES=1024
CONFIG_NAWSERVER_TCP_ADMISSION_RESPONSE_BYTES=4096
CONFIG_NAWSERVER_TCP_ADMISSION_STREAM_BYTES=12288
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=2
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=131072
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=2
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1553056 байт**; linker total_size: **1552933 байт**; статические data+bss: **63640 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 937470 | 0 | 0 |
| Flash Data | 468056 | 0 | 0 |
| DIRAM | 167103 | 341760 | 174657 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-55-3333963DA019-29CE539ED691 -->
### 2026-10-27 16:07:10 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 55

Исходники: `632dafd` + managed memory initial/dynamic + API reserve + keep-alive, текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/55`; SDKCONFIG SHA256-prefix `3333963DA019`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_TCP_ADMISSION is not set
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **976304 байт**; linker total_size: **975932 байт**; статические data+bss: **38384 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 736574 | 0 | 0 |
| Flash Data | 153332 | 0 | 0 |
| DRAM | 111230 | 321296 | 210066 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-53-B44DFC83DDB1-201572068F4B -->
### 2026-10-27 16:16:38 +05:00 — NaWServer_c6_lcd147, esp32c6, SDK 5.5.6, профиль 53

Исходники: `632dafd` + managed memory initial/dynamic + API reserve + keep-alive, текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/53`; SDKCONFIG SHA256-prefix `B44DFC83DDB1`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_ADMISSION=y
CONFIG_NAWSERVER_TCP_ADMISSION_HEAD_BYTES=1024
CONFIG_NAWSERVER_TCP_ADMISSION_RESPONSE_BYTES=4096
CONFIG_NAWSERVER_TCP_ADMISSION_STREAM_BYTES=12288
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=131072
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **1350544 байт**; linker total_size: **1350186 байт**; статические data+bss: **66312 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 1202264 | 0 | 0 |
| DIRAM | 192374 | 452112 | 259738 |
| LP SRAM | 56 | 16384 | 16328 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-54-D7B211F22102-404178365BF9 -->
### 2026-10-27 16:16:38 +05:00 — NaWServer_touch_lcd7, esp32s3, SDK 5.5.6, профиль 54

Исходники: `632dafd` + managed memory initial/dynamic + API reserve + keep-alive, текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/54`; SDKCONFIG SHA256-prefix `D7B211F22102`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED=80
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=1000
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
CONFIG_NAWSERVER_TCP_ADMISSION=y
CONFIG_NAWSERVER_TCP_ADMISSION_HEAD_BYTES=1024
CONFIG_NAWSERVER_TCP_ADMISSION_RESPONSE_BYTES=4096
CONFIG_NAWSERVER_TCP_ADMISSION_STREAM_BYTES=12288
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=2
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=131072
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=2
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

BIN: **1553072 байт**; linker total_size: **1552953 байт**; статические data+bss: **63640 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 937490 | 0 | 0 |
| Flash Data | 468056 | 0 | 0 |
| DIRAM | 167103 | 341760 | 174657 |
| IRAM | 16384 | 16384 | 0 |
| RTC SLOW | 32 | 8192 | 8160 |
| RTC FAST | 24 | 8192 | 8168 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.

<!-- backend26-55-3333963DA019-3983158162B4 -->
### 2026-10-27 16:16:38 +05:00 — NaWServer_minimal, esp32c3, SDK 6.1.0, профиль 55

Исходники: `632dafd` + managed memory initial/dynamic + API reserve + keep-alive, текущие MCP/JSON/demo (27.10.2026); build `local/backend26-idf/55`; SDKCONFIG SHA256-prefix `3333963DA019`.

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_PARTITION_TABLE_MD5=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_HZ=100
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_TCP_ADMISSION is not set
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

BIN: **976320 байт**; linker total_size: **975940 байт**; статические data+bss: **38384 байт**.

| Область linker map | Used, байт | Total, байт | Free, байт |
|---|---:|---:|---:|
| Flash Code | 736582 | 0 | 0 |
| Flash Data | 153332 | 0 | 0 |
| DRAM | 111230 | 321296 | 210066 |
| RTC SLOW | 60 | 8192 | 8132 |

Runtime heap: пока не измерен для этого образа. Результат относится к сборке.


### 2026-10-27 16:24 +05:00 — managed memory + keep-alive, C6/S3

Отчёт с точными ELF/boot/сырьём и сохранёнными FAIL:
[27.10.2026: менеджер и keep-alive](readme_20261027_memory_manager_keepalive_ru.md).
TCP+bounded+MCP, SDK5.5.6, profiles53/54; основа632dafd + рабочая ревизия.

```text
http_memory.enabled=true
initial_requests=2, maximum_requests=9, initial_websocket_clients=0
reserve_api_buffers=true; primary HTTP1/WS0/SSE0 rate8/1000ms
shared identities=2, each HTTP4/WS1/SSE1 rate12/1000ms, identity idle5000ms
TCP budget=65536, floor=32768 bytes (explicit app override; sdkconfig default131072)
TCP/SSE placement: C6 System/System, S3 PsramOnly/PsramOnly
SSE budget=16384, initial_clients=0, floor=0
HTTP head/body/response/head-out=1024/4096/4096/512; max_headers32
keep_alive: enabled, idle slots4, idle5000ms, requests100; primary idle lane protected
ingress4, registry14, sockets24; header500ms, refusal250ms, I/O slice1024
close grace250ms, drain8192 bytes; no SHUT_WR before native close
JSON input4096/workspace2048/MCP output2048; caller-owned
```

| Сравнение учтённых TCP массивов | До | После |
|---|---:|---:|
| При старте/после освобождения, Б |128531|28951 |
| Предельный byte budget, Б |131072|65536 |
| Пик реального byte-pressure, Б |Не эта серия|63767 |
| Body общих GET / WS / SSE payload без клиента, Б |Ранее заранее резервировались|0 /0 /0 |

| Проверка | C6 | S3 |
|---|---|---|
| Первая managed серия REST1/2/3, по60с |60/120/180;0 ошибок|60/120/180;0 ошибок |
| Финальный образ REST1/2/3, по30с |30/60/90;0 ошибок|30/60/88; на3 клиентах1 header timeout |
| S3 повтор3 клиентов,60с без reset/смены bounds |—|180/180;0 ошибок |
| Минимум sampled internal обычные1/2/3 финальной серии, Б |205380/207260/207296|85315/86923/78507 |
| Keep-alive probe |22 запроса/1 TCP PASS|22 запроса/1 TCP PASS |
| Реальный byte pressure: до/во время/после, Б |28951/63767/28951|28951/63767/28951 |
| Лишние POST503 / max отказа, мс |2/35|2/12 |
| Финальный QoS30с: primary/keeper |60/60;30/30|60/60;30/30 |
| Финальный QoS503/429; ошибки/timeout |300/547;0/0|298/546;0/0 |
| WS/SSE extras / рабочий WS |10+10 полных503;10/10 pong|10+10 полных503;10/10 pong |
| После всех прогонов internal free/largest, Б |207380/167936|87079/40960 |
| SDK historical internal minimum, Б |156488|33340 |
| PSRAM после / SDK historical minimum, Б |—|5362436/5324192 |
| HTTP task stack min free, Б |1960|1380 |
| BIN / static data+bss, Б |1350544/66312|1553072/63640 |

Важно: в первом финальном C6 QoS1 гостевой dial завершился локальной Windows
ошибкой connectex WSAEADDRINUSE, primary60/60 и timeout0. Повтор прошёл, исходный
FAIL сохранён. В S3 первый финальный3-client REST имел1 header timeout; RSSI−74,
boot stable/recovery200, UART без panic/reset/disconnect, свободная память после
87347 internal/5362436 PSRAM. Причина не установлена; успешный повтор не отменяет
ошибку и не даёт полной безошибочной приёмки S3. Первый guard pressure проверил
только квоты из-за занятой общей идентичности; после усиления harness отдельный
повтор подтвердил именно достижение byte budget. Firmware для повторов не меняли.

Floor/probe no-attempt и отказ неисправного probe проверены fake allocator на
host; принудительный OOM всей платы не запускался. SDK minimum — сумма minima
heap regions, не одновременный минимум всей internal RAM. Порог S3 PsramOnly
относится к PSRAM; SDK/Wi-Fi/LCD и internal heap им не ограничиваются.

Windows54/54, Linux7/7 PASS; host feature OFF3/3 из основной серии.
C3 SDK6.1.0 scheduler OFF: build-only, BIN976320 Б, static data+bss38384 Б.
Все отдельные нагрузки30/60с, pressure~1,5с, stream~3,5с; RAM observers отдельно.
Boot stable внутри каждой ревизии; между ревизиями намеренная прошивка/reset.
Динамические body/WS/SSE освобождены, ledger28951 Б. Эксперименты выключены;
managed load/observers/UART закрыты. Окружение не обновлялось; commit/push нет.

### 2026-10-27 — реальная память в INFO и верхнем индикаторе

[Распределение RAM и визуальная приёмка](readme_memory_distribution_ru.md).
Три Vue-профиля build PASS; frontend24/24, host54/54. C6/S3 SDK5.5.6 прошиты
после chip/MAC/range gate, vendor verify PASS, API ELF MATCH. C3 showcase
SDK6.1.0 — build-only с новым collector/embedded frontend, без прошивки.
SDK, пакеты, сеть ПК не обновлялись. Изменение UI пользователя учтено:
RAM/PSRAM с объёмом сбоку столбиков, без надписей сверху/снизу;48×108 px.

| Последний runtime sample, Б | C6 internal | S3 internal | S3 PSRAM |
|---|---:|---:|---:|
| Всего учтено (heap + статика данных) |411916|358683|6934624|
| Свободно |207216|79431|5343628|
| Наша статика main+NaWServer |27384|21944|0|
| Прочая статика |38936|41696|0|
| Живые payload буферы нашего allocator |28999|0|44375|
| Остальная динамика/служебное |109381|215612|1546621|

Это5-я запись из5 последовательных summary каждой платы; сумма категорий
равна Всего. Статика — реальные linker boundaries образа; heap и собственные
выделения — runtime SDK/атомарные счётчики по адресу. Снимки не синхронизируют
все задачи. Остальная динамика не выдаётся за полную атрибуцию стороннего кода.
DMA отдельно не суммируется; IRAM/RTC/ROM/flash/недоступные резервы не добавлены.
Прежний график RAM продолжает использовать heap-only масштаб.

Промежуточный C6 CBOR map-count500 исправлен, финальные HTTP summary5/5,
валидность схемы/баланса/стабильный boot PASS. Сырые файлы:
local/memory27/{c6,s3}-ui-side-measured.json и
local/night26/memory27-ui-side-{c6,s3}.json. Браузерная проверка реальных C6/S3 PASS.
Host8788 пересобран без restore/install и перезапущен только для исправления
404 новых hashed JS после обновления frontend; HTML/JS200. Синтетический uptime
модели сброшен. Дополнительная S3 QA-вкладка закрыта; preview8800 оставлен для
просмотра. Commit/push не делались.

## 28.10.2026 — очистка имени и карта слоёв

Активные исходники, имена и ссылки документов и проект SeMaps переименованы;
реальные старые журналы, кеши и Git history не переписывались. Архитектура TCP и
HTTPD сверена с текущим кодом и показана в readme_architecture_ru.md. Имеющаяся
карта SeMaps ещё не обновлена: доступный в этой сессии MCP подключён к другой
рабочей области, C++ extractor здесь не настроен.

C3/C6/S3 пересобраны и прошиты с проверкой чипа, границ и записи; IP и ELF
подтверждены, JS/CSS HTTP 200. Локальные Wi-Fi значения сохранены. Frontend
24/24, C++ 54/54, simulator 84 проверки, semaps check без замечаний.

## 30.10.2026 — публикационный снимок после Uncrustify 0.83.0_f

Чистые SDK-сборки minimal, без credentials, прошивки и runtime-замеров.
Проверено HTTPD/TCP + WS/SSE; MCP/reader отмечен отдельно. Все шесть профилей PASS.
Статика — сумма `.data`/`.bss` по vendor esp_idf_size 2.2.1, не GNU size и не runtime RAM.

### 0-esp32c3-5.5.6-HTTPD

BIN: **532624 Б**; статические data+bss: **28156 Б**; SDKCONFIG SHA256-prefix: `85e5d31db053`.

```ini
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

Runtime heap/минимумы/PSRAM: не измерялись, образ не прошивался.

### 1-esp32c3-5.5.6-TCP

BIN: **543104 Б**; статические data+bss: **32420 Б**; SDKCONFIG SHA256-prefix: `807b56f6c834`.

```ini
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_TCP_ADMISSION is not set
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

Runtime heap/минимумы/PSRAM: не измерялись, образ не прошивался.

### 2-esp32c3-6.1-HTTPD

BIN: **510704 Б**; статические data+bss: **29583 Б**; SDKCONFIG SHA256-prefix: `359a3dff1013`.

```ini
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_NAWSERVER_BACKEND_HTTPD=y
# CONFIG_NAWSERVER_BACKEND_TCP is not set
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_HTTPD_STACK_SIZE=6144
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
# CONFIG_NAWSERVER_MCP is not set
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

Runtime heap/минимумы/PSRAM: не измерялись, образ не прошивался.

### 3-esp32c3-6.1-TCP

BIN: **521280 Б**; статические data+bss: **40039 Б**; SDKCONFIG SHA256-prefix: `08e7e49a7003`.

```ini
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_COMPILER_CXX_GLIBCXX_CONSTEXPR_NO_CHANGE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_TCP_ADMISSION is not set
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
# CONFIG_NAWSERVER_JSON_YYJSON is not set
CONFIG_NAWSERVER_JSON_RAPIDJSON=y
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=8192
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

Runtime heap/минимумы/PSRAM: не измерялись, образ не прошивался.

### 4-esp32c6-5.5.6-TCP

BIN: **736320 Б**; статические data+bss: **53756 Б**; SDKCONFIG SHA256-prefix: `a3b64cda1145`.

```ini
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_TCP_ADMISSION is not set
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
# CONFIG_NAWSERVER_JSON_BOUNDED is not set
CONFIG_NAWSERVER_JSON_YYJSON=y
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=16384
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

Runtime heap/минимумы/PSRAM: не измерялись, образ не прошивался.

### 5-esp32s3-5.5.6-TCP

BIN: **522512 Б**; статические data+bss: **32320 Б**; SDKCONFIG SHA256-prefix: `323a4d445b00`.

```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_TCP_ADMISSION is not set
CONFIG_NAWSERVER_TCP_MEMORY_POLICY=0
CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536
CONFIG_NAWSERVER_SSE_MEMORY_POLICY=0
CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384
CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE=8192
CONFIG_NAWSERVER_WEBSOCKET=y
CONFIG_NAWSERVER_SSE=y
CONFIG_NAWSERVER_MCP=y
CONFIG_NAWSERVER_JSON_BOUNDED=y
# CONFIG_NAWSERVER_JSON_YYJSON is not set
# CONFIG_NAWSERVER_JSON_RAPIDJSON is not set
CONFIG_NAWSERVER_JSON_INPUT_BYTES=4096
CONFIG_NAWSERVER_JSON_STRING_BYTES=512
CONFIG_NAWSERVER_JSON_CONTAINER_ITEMS=32
CONFIG_NAWSERVER_JSON_DEPTH=8
CONFIG_NAWSERVER_JSON_VALUE_CAPACITY=256
CONFIG_NAWSERVER_JSON_WORKSPACE_BYTES=2048
CONFIG_NAWSERVER_MCP_OUTPUT_BYTES=2048
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

Runtime heap/минимумы/PSRAM: не измерялись, образ не прошивался.

# Подготовка P6: ESP-IDF TCP adapter и выбор backend

Дата: 2026-10-25. Срез локальных файлов: 07:05 UTC. Статус: **read-only handoff,
не утверждённая реализация P6**. При поручении записи этой заметки длительные
проверки P5 ещё выполнялись; их завершение и checkpoint эта заметка не подтверждает.
P6 следует после checkpoint P5 в рамках исходного поручения владельца выполнять
P0–P7 последовательно; отдельное разрешение на каждый этап не требуется.

В этом проходе не выполнялись SDK-сборки, SDK-установка, запуск прошивки или
операции с контроллером. Наличие заголовков, старого build directory или
compiler-only объекта не является новой проверкой SDK/runtime/hardware.

Основания:

- [ADR общего TCP engine](../adr/ADR_20261024_nawserver_shared-tcp-http-engine.md).
- [Исследование](../reviews/2026-10-24_shared-tcp-http-engine.md).
- [Контракты](PLAN_20261024_nawserver_tcp-engine-contracts.md).
- [Основной план, P6 и критерии приёмки](PLAN_20261024_nawserver_tcp-engine-implementation.md).
- [Текущий отчёт](../readme_tcp_engine_verification_ru.md).

## 1. Что сейчас есть в компоненте

Корневой `CMakeLists.txt` по-прежнему безусловно собирает native HTTPD backend,
`httpd_response.cpp`, фасад `Server`, native WS и native SSE adapter. В нём
`REQUIRES esp_http_server`, `PRIV_REQUIRES esp-tls esp_timer`. Корневых `Kconfig`
и `Kconfig.projbuild` в проверенном срезе нет. SHA-256 `CMakeLists.txt`:
`61147DE6F3A01F54C1503CF666C9187B748E70D8031270AD96C9ECB027DD8FF3`.

Общие `HttpServer`, `HttpExchange`, Request/Response, middleware и маршруты уже
не требуют HTTPD. Но `include/NaWServer/server.h` включает `backends/esp_idf.h`,
владеет `EspIdfHttpBackend` и предоставляет `httpd_handle_t native_handle()`.
Это native HTTPD facade; его нельзя незаметно переопределять под TCP.
TCP-приложение должно явно компоновать `HttpServer`, `TcpHttpBackend` и
конкретный IDF transport; SSE — через `sse::TcpAdapter`.

В собственных проверенных `src/` и `include/` нет вызовов esp_tls/mbedtls/PSA.
Поэтому прежний прямой `esp-tls` dependency нельзя автоматически переносить в
TCP branch. Wi-Fi или другие компоненты приложения могут независимо требовать
crypto; отсутствие HTTPD проверяется по dependency graph/map, а не отсутствием
всех crypto symbols в произвольной Wi-Fi прошивке.

## 2. Реальная доступность SDK

Версии ниже прочитаны из `components/esp_common/include/esp_idf_version.h`.
Это инвентаризация файлов, не результат новой сборки.

| Путь | Версия заголовка | Назначение в P6 |
|---|---|---|
| `%USERPROFILE%/.platformio/packages/framework-espidf@3.50400.0` | 5.4.0 | Точный SDK исходной строки matrix доступен |
| `%USERPROFILE%/.platformio/packages/framework-espidf@src-9531ca4433dfe754a3e12887170ce9d5` | 5.4.2 | Не заменяет 5.4.0 |
| `%USERPROFILE%/.platformio/packages/framework-espidf` | 6.1.0 | Не заменяет 6.0.3 |
| `<sdk-root>/v5.4.4/esp-idf` | 5.4.4 | EIM installation; не заменяет 5.4.0 |
| `<sdk-root>/v6.1/esp-idf` | 6.1.0 | EIM installation; не заменяет 6.0.3 |

SHA-256 version headers соответственно:

```text
5.4.0  D1B0CE9C7C36D2A9B87689D75E3BB8887C7230F43AA2B1320D1B830745A20DDD
5.4.2  84C57A8D9A2AA0AE6186D86C1B16F79865E91C8991A9AB3E022149584CB0657C
6.1.0  9FB8B45CC85D411E02F024CC06B818C031C3E33614E50F08905F1980254D3449
5.4.4  D202C275E326B6AEFDB32FAAB4125773ECECB6A2C3C8344368BF261256B1F2CE
```

В проверенных PlatformIO packages, зарегистрированных EIM installations и
локальных SDK inventory не найден установленный 6.0.3. Исторический
`local/idf-dual-sdk/readiness.json` относится к 5.4.4/6.1 hello_world; это не
приёмка NaWServer под 5.4.0/6.0.3. Профиль `examples/minimal/platformio.ini`
фиксирует `espressif32@6.10.0`, framework `3.50400.0` и RISC-V compiler
`14.2.0+20241119`; заменять framework внутри этого pin нельзя.

[Официальный релиз v6.0.3](https://github.com/espressif/esp-idf/releases/tag/v6.0.3)
существует, короткий commit tag — `76f5ded`. Espressif предоставляет
[архив с submodules](https://dl.espressif.com/github_assets/espressif/esp-idf/releases/download/v6.0.3/esp-idf-v6.0.3.zip).
Для будущей строки 6.0.3 нужны отдельные точные SDK/tool environment и build
directories. В этом проходе архив не скачивался. Совместимость не выводится из
установленного 6.1; исходники 6.0.3 ещё требуется прочитать и проверить.

## 3. Предлагаемая минимальная граница файлов и build profiles

Новые файлы-кандидаты: `include/NaWServer/backends/idf_tcp.h`,
`src/backends/tcp/platform/idf_tcp.cpp`, `src/backends/tcp/platform/idf_worker.cpp`.
Transport остаётся узким concrete implementation существующего `tcp::Transport`.
Инициализация Wi-Fi, netif и credentials остаётся у приложения; конструкторы не
запускают SDK resources. Универсальная POSIX/RTOS HAL не требуется.

Корневой Kconfig должен отдельно выбрать HTTPD или TCP; default HTTPD сохраняет
нынешнюю композицию. Названия новых WS/SSE feature flags и их defaults ещё
нужно согласовать. Native WS следует связывать с `CONFIG_HTTPD_WS_SUPPORT`;
TCP WS от этого HTTPD symbol зависеть не должен. Нужны четыре явных профиля:
HTTP-only, WS без SSE, SSE без WS, WS+SSE — для обоих backend и обоих точных SDK.

| Группа | Предлагаемые sources/dependencies |
|---|---|
| Common | Общие HTTP Request/Response/dispatcher, handlers/static, auth, routing, URL/form |
| WS common, когда включён | `src/websocket/handler_ws.cpp`; shared UTF-8 helper остаётся независимым от SSE |
| SSE common, когда включён | encoder/backend/response_bridge/event_source |
| HTTPD | `web_server.cpp`, `httpd_response.cpp`, `backends/esp_idf.cpp`; native WS/SSE adapters только при соответствующей feature; `esp_http_server` |
| TCP HTTP | wire parser/writer, session_io/http_connection/connection_registry/http_backend, IDF socket+worker; private SDK dependencies lwip/freertos/esp_timer |
| TCP WS | wire handshake/frame codec, websocket_connection, websocket_tx_pool |
| TCP SSE | queued_adapter, sse_stream, tcp_sse |

Одного разделения CMake lists недостаточно. Сейчас `http_backend.cpp`
безусловно использует WS classes и `TcpSseExchange` в Bindings;
`ConnectionRegistry` включает `sse_stream.h`, наследует `TcpSseLeaseProvider`
и содержит SseLease implementation. Здесь потребуются узкие private project
feature guards. Отключённая feature не должна оставлять недостающие symbols или
скрыто подтягивать её runtime. Такие guards не являются SDK version branches.
Различия версий SDK, если они реально обнаружатся, остаются внутри IDF adapters;
спекулятивные `ESP_IDF_VERSION` branches в wire engine не добавлять.

Проверять отдельное minimal standalone приложение: HTTPD отсутствует в TCP-only
map/dependencies, TCP wire отсутствует в HTTPD-only. Root frontend общий,
приложение добавляет профиль и состав routes, не копии frontend implementation.

## 4. Что подтверждено исходниками lwIP 5.4.0

База всех путей этой секции:
`%USERPROFILE%/.platformio/packages/framework-espidf@3.50400.0/`.

Нужны обычный TCP socket и явный `FIONBIO` для listener **и accepted socket**.
Linux `accept4`, `SOCK_NONBLOCK|SOCK_CLOEXEC` сюда не переносить. Предпочтительны
публичные `lwip_*` API из `lwip/sockets.h`, чтобы не расширять границу до VFS.
Ни private RAW TCP API, ни прямой `tcp_abort` из нашего adapter не разрешены.

`lwipopts.h:969,981` отключает compat/POSIX IO name macros;
`lwipopts.h:993` задаёт `LWIP_SOCKET_OFFSET` через SDK fd/socket capacities.
`lwip_poll` объявлен в `lwip/sockets.h:623`; caller output mapping и duplicate
cardinality остаются общими. Stack scratch можно ограничить SDK socket capacity,
а не безусловными 64 entries. В прочитанном C3 config 10 sockets, FD offset 54;
это не означает, что generic valid handle 0 является IDF socket.
Backlog и SDK socket budget не равны числу browser clients.

`lwip/sockets.h:274` помечает `MSG_NOSIGNAL` как unimplemented; Linux signal
policy сюда не переносится. Для I/O использовать проверенный nonblocking режим
и поддерживаемый `MSG_DONTWAIT`. Внутренние SDK locks и scheduler этим не исчезают.

### 4.1 Close и shutdown: необходимое решение до реализации

| Место SDK 5.4.0 | Прочитанное поведение |
|---|---|
| `components/lwip/lwip/src/api/sockets.c:817` | `lwip_close`: ошибка `netconn_prepare_delete` возвращается **до** `free_socket`; нельзя копировать Linux release-on-any-error |
| `components/lwip/lwip/src/api/api_lib.c:195` | prepare-delete отправляет SDK API message и ожидает результат |
| `components/lwip/lwip/src/api/sockets.c:2737` | `lwip_shutdown` вызывает `netconn_shutdown`; сам ownership fd не освобождает |
| `components/lwip/lwip/src/api/api_lib.c:1107` | shutdown тоже сохраняет начало timeout и вызывает close path |
| `components/lwip/lwip/src/api/api_msg.c:969,1074` | общий close/shutdown path может ждать при нехватке памяти для FIN, даже для nonblocking netconn |
| `components/lwip/lwip/src/include/lwip/opt.h:2127` | default close timeout — 20000 ms |
| `components/lwip/lwip/src/api/api_msg.c:1081–1104` | положительный `send_timeout` заменяет default; linger может его переопределить; при pressure timeout SDK close может abort TCP и вернуть `ERR_OK` |
| `components/lwip/lwip/src/api/api_msg.c:1147` | отложенный close проверяется TCP poll callback с интервалом 500 ms |

Таким образом, `O_NONBLOCK` сам по себе не доказывает bounded close/shutdown.
Внешний exchange deadline или middleware не прерывает уже выполняющийся SDK call.
Успешный native close также не является доказательством доставки последних bytes.
Последовательность shutdown → close может включать отдельные SDK waits; нынешний
50 ms WS trailing-drain deadline не ограничивает их суммарную длительность.

Кандидаты для отдельного решения и проверки, **ни один здесь не выбран**:

- Положительный конечный `SO_SNDTIMEO` может ограничить SDK FIN-memory pressure
  wait. Это не обещание завершения за заданные 1/20/50 ms: остаются примерно
  500 ms poll granularity, SDK processing и scheduler tolerance. Нужны реальный
  SDK failure path и измерение на C3 под обоими точными SDK.
- Positive linger на nonblocking socket может вернуть retained WouldBlock;
  его нельзя незаметно превратить в потерянный handle или бесконечно занятый slot.
- `SO_LINGER {on,0}` отбрасывает unsent data и вызывает reset. Это **не выбранная
  normal-close policy**; скрытый default linger0/abort запрещён. Наш adapter не
  должен вызывать private RAW `tcp_abort` ради обхода public socket API.

Для результата close: native success освобождает owner; EBADF требует явной
политики already-gone; остальные ошибки нельзя автоматически объявлять released.
Классификацию известных retained/retryable ошибок, включая WouldBlock/InProgress,
следует проверить в точных SDK. Неизвестный retained Fatal сохраняет зависимости;
слепой retry недопустим. Общий Registry уже хранит такой outcome. Не скрывать
native error под успешным stop и не обещать recoverable global SDK OOM.

## 5. Worker, TLS и память

Публичный SDK surface: FreeRTOS task API, `esp_timer_get_time()` и публичные lwIP
thread/socket helpers. Один HTTP/WS task, отдельный SSE task только после явного
start EventSource/TcpAdapter. Task на каждого клиента и scheduler framework не нужны.

`freertos/task.h:315–316` определяет stack depth в **байтах**, в отличие от
vanilla FreeRTOS. Текущий native backend задаёт 6144 B HTTPD
(`src/backends/esp_idf.cpp:112`), native SSE — 4096 B, priority 4
(`src/sse/adapter.cpp:55`); HTTPD default priority — idle+5.
Кандидат private worker roles: HTTP/WS 6144 B и SSE 4096 B, priorities 5/4.
Это исходная конфигурация для обсуждения, **не измеренная достаточность TCP stack**.
Если нужен больший стартовый профиль, каждый KiB увеличить явно, объяснить и
проверить high-water/headroom C3 вместе с callbacks и SDK. Host stack reservation
и compiler frame size не заменяют эту проверку.

TLS identity выставлять до вызова entry, не читать ещё не опубликованный native
TaskHandle. Entry не должен возвращаться из TaskFunction. Private completion/
parking/join protocol обязан исключить обращения task к освобождённым Worker,
context и primitives. Successful join означает отсутствие дальнейшего исполнения
библиотечного entry; timeout сохраняет owner/dependencies. Self-join запрещён.
Нельзя подменять cooperative stop внешним удалением task внутри работающего callback.

`lwipopts.h:941` включает semaphore per thread. `sys_arch.c:477,505` лениво
выделяет storage pointer и FreeRTOS binary semaphore при первом socket API;
`sys_arch.c:489` освобождает их через pthread TLS cleanup.
`sockets.c:359,366` предоставляет `lwip_socket_thread_init/cleanup`.
Учитывать эти SDK allocations отдельно от нашего `new` counter. У library-owned
worker cleanup/prewarm можно рассмотреть в его startup/exit protocol; app-thread
TLS не принадлежит Transport и не должен очищаться за другие SDK components.
Фактическое освобождение task stack/TCB/TLS и возможную работу idle task проверять,
а не выводить из одного completion flag.

Сохранённый P0 profile
`local/tcp-engine-p0-c3/sdkconfig.c3_httpd_full:1154` задаёт 100 Hz tick;
это не утверждение о всех demo profiles или новой прошивке. При таком выбранном
config положительная 1 ms private sleep должна округляться хотя бы до одного tick.
Deadline брать от монотонного `esp_timer_get_time()/1000`, отдельно учитывать
фактические tick/scheduler tolerance нового профиля.

## 6. Отпечатки прочитанных SDK sources

Все hashes SHA-256; paths относительно базы SDK 5.4.0 из секции 4. Line numbers
относятся только к этим bytes; не переносить их автоматически на 6.0.3/6.1.

| Source | SHA-256 |
|---|---|
| `components/lwip/lwip/src/api/sockets.c` | `861822D05AC60F68228279036A966C26ACB1282B8F2F7BD0C4672129C7FD74A4` |
| `components/lwip/lwip/src/api/api_lib.c` | `526D1E90E2BECBDEAB970DD712665A7DD2285B12BDD9089F17ED4330E77C477E` |
| `components/lwip/lwip/src/api/api_msg.c` | `A793B371032BA38CA5E23FD7FEDE4C910AEA0079FFE612D781109F203EAB92A0` |
| `components/lwip/lwip/src/include/lwip/opt.h` | `B480E913C964961C809EEC8F91FB3187A91F2638D2FBA682B80AEDB770AF1DA5` |
| `components/lwip/lwip/src/include/lwip/sockets.h` | `CD8AF60D7ACD3158103CD2B141884CECEA79D4011DAAA6C9E3F987BB21F8AD84` |
| `components/lwip/port/include/lwipopts.h` | `343263B1136B8D0EB0A43B22D1FF1A58A50C1FEFBC11014C52AA70632D3D1AE3` |
| `components/lwip/port/freertos/sys_arch.c` | `304978233EC040C5F0E95E1A08209C038B5E03695638E1640301FA1F07C307B2` |
| `components/freertos/FreeRTOS-Kernel/include/freertos/task.h` | `AC666BCBE4ACB21C44CC820631F8C579D089698967015054C5AB42D092DDDC2D` |

## 7. Последовательность после разрешения P6

1. Зафиксировать checkpoint P5. Получить точный SDK 6.0.3 с его штатными tools;
   заново прочитать socket/close/task contracts, не используя 6.1 как замену.
2. Выбрать и документировать public-API-only bounded close/shutdown policy с
   возможными retained outcomes и потерей error-frame delivery при pressure.
3. Согласовать backend/feature gates и private worker roles; затем реализовать
   только concrete IDF transport/worker и необходимые feature boundaries.
4. SDK-сборки запускать в фоне, в отдельных directories, для C3 и точных
   5.4.0/6.0.3. Проверить четыре feature profiles, link/dependency exclusion,
   allocation/stack ledger и rollback. Эти действия здесь ещё не выполнялись.
5. Hardware — отдельный этап: повторная идентификация разрешённой C3 и intended
   write ranges перед каждой прошивкой; без backup и дополнительных post-flash
   readback/hash passes. Никакой прошивки эта заметка не подтверждает.

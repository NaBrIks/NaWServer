# Проверка Request/Response на C3 — 19 октября 2026

## После выделения общего WS — 24 октября 2026

HTTP checkpoint сохранён коммитом5a09364; последующий WS этап описан в
[контракте и отчёте адаптеров](readme_ws_adapters_ru.md). Общий HTTP путь и
утилиты сохранены. Полный host CTest16/16 PASS, Windows browser14/14 и repeat PASS.
На C3 с новым WS backend: SDK build PASS, startup89/63/27 checks PASS,
LAN smoke с WS/reconnect PASS,198 HTTP checks PASS; browser14/14 и repeat PASS.
На каждой платформе HTML/CSS/JS общие, CSS отдельным ресурсом. Results ниже
относятся к предыдущим этапам; их прежние ограничения не описывают новый WS этап.

## Итог HTTP backend и реальная C3 — 24 октября 2026

Проверена именно examples/minimal / c3, pinned ESP-IDF5.4.0, RISC-V GCC14.2,
C++17, CONFIG_HTTPD_WS_SUPPORT=y. Чип/порт повторно идентифицированы перед
записью, app0 размещение подтверждено по metadata существующей таблицы.
Записан только app0 по0x10000, размер921904 bytes меньше0x280000. Bootloader,
таблица разделов, NVS/otadata/data не записывались, backups не делались.
Отдельного post-flash hash/readback не было; использовано штатное поведение esptool.

- Реальное выполнение на C3: 89 response/basic checks PASS, Response=952 bytes;
  selftest63/63, lifecycle27/27 за три end/begin цикла, сервер оставлен running.
  Проверены повторные begin/end, отсутствие listener после end, HTTP/auth и
  WS text/binary4096 после каждого восстановления. Producer ещё не запущен
  во время этих тестов; остановка выполняется из app_main, не из URI callback.
- LAN existing tools/smoke: все4 группы PASS (HTTP/form/auth/body/binary,
  WebSocket text/binary/reconnect). tools/http_api_smoke: 198 HTTP checks PASS,
  включая 2/4 concurrent clients. Исходники этих клиентов не менялись.
- C3 Edge browser:14/14 PASS и повторный прогон PASS; общий mini UI,
  отдельный CSS, WS text и binary/NUL. Проверены светлая/тёмная темы и430/1180px.
  SDK request header limit minimal=1024; сначала browser WS получал431 при512,
  Go WS с короткими headers уже работал. Изменена конфигурация примера,
  endpoint/WS protocol implementation не менялись ради этого результата.
- Windows: CTest14/14 PASS, browser12/12 PASS; Windows HTTP test дополнительно
  20 последовательных запусков PASS. Native TCP close делает bounded drain
  после FIN, чтобы непрочитанный остаток отклонённых headers не превращал431
  в reset. Assertion проверяет весь ответ431, а не только status prefix.
- Один диагностический resource snapshot после LAN checks: free197528,
  minimum178280, largest114688 bytes; HTTP stack minimum free2012 bytes.
  Это короткая acceptance, не load/soak и не доказательство zero-heap.

Адрес, device identity, credentials, firmware и raw serial evidence только в
ignored local/idf-c3-hardware/. Итог: ui-acceptance.json, serial-final.log,
build-final.log, upload-final.log; screenshots в browser/. Wi-Fi использует
разрешённую владельцем локальную ignored конфигурацию, её содержимое не публикуется.
Проверка ниже остаётся исторической для указанных там ревизий/дат.

## Host backend experiment — 24 октября 2026

Рабочая ветка `codex/http-backend-adapters`, исходный checkpoint `14b6e74`.
Это новая host-проверка; результаты C3 ниже относятся к исторической ревизии.

- Zig 0.14.1 C++17, Ninja: общий HTTP core, memory и Windows backend собраны.
  Общая библиотека не использует SDK headers/stubs. Core/backend компилируются
  с warnings-as-errors. SDK stubs остаются только у прежнего native WS harness.
- CTest: 12/12 PASS. Новые тесты проверяют полный dispatcher, binary/NUL,
  частичное/оборванное чтение, timeout/send failure, body limit, query/form,
  auth/permissions, 404/500, повторную отправку и lifecycle.
- Настоящие Windows TCP-сокеты: headers/framing, duplicate/invalid Content-Length,
  CL+TE, unsupported chunked/Expect, HEAD, limits, остановка с незавершённым
  запросом и повторный запуск. Connection: close, IPv4 loopback only.
- Настоящий Edge через Playwright: 12/12 browser fetch PASS и повторный запуск PASS;
  ошибок JavaScript нет. CSS загружен отдельно с text/css. Светлая/тёмная темы
  и viewport 1180/430 px проверены визуально по скриншотам. Узкая таблица имеет
  внутреннюю горизонтальную прокрутку, overflow всей страницы отсутствует.
  Скрипт: `tests/browser_http_check.cjs`, evidence: ignored `local/http-host-build/`.
- Windows demo оставлено на 127.0.0.1:8080. Пример credentials test/test-only —
  только тестовые данные локального host demo.

URL-decoder, form parser, CBOR и мелкие утилиты не изменялись этим экспериментом.
Примеры мигрированы только в местах типов HttpMethod и нативного SDK-доступа.
IDF Server перенаправлен на общий dispatcher на уровне исходников. Последующее
выделение lifecycle описано ниже. По указанию владельца ESP32-сборки,
прошивка и работа с подключённой C3 в этой сессии не выполнялись.

## IDF lifecycle extraction — 24 октября 2026

Полный host CTest после выделения: **14/14 PASS**, Zig 0.14.1 C++17 / Ninja.
Оба lifecycle target собраны с warnings-as-errors при CONFIG_HTTPD_WS_SUPPORT=0/1.
Проверка форматирования затронутых lifecycle sources PASS.

EspIdfHttpBackend реализует HttpBackend и владеет HTTPD handle/контекстами callbacks.
Существующий Server делегирует begin/end/native_handle; общий HTTP dispatcher,
парсеры, утилиты, WS sender и SSE runtime этим выделением не меняются.
Сохранены native config, шесть wildcard методов и WS registration перед HTTP.
После ошибки регистрации выполняется откат; если stop также неуспешен, ресурсы
сохраняются для повторной остановки. Общий API сообщает CleanupRequired.

Host-only стенды `idf_lifecycle_0` / `idf_lifecycle_1` компилируют настоящие
src/backends/esp_idf.cpp, src/http/web_server.cpp и httpd_response.cpp с управляемой
SDK-границей из tests/idf_stubs. Проверяются конфигурация, inert constructors,
normal/repeated start/stop, вызов общего HTTP dispatcher с binary/NUL body,
ошибка SDK start, ошибка каждой из шести регистраций, успешный/неуспешный откат,
повторный end, сохранность callback contexts при stop failure и во время stop,
WS registration/dispatch при WS=1 и явный отказ при WS=0, static/mixed route checks.

Это host-проверки владения и ветвей ошибок, не доказательство работы настоящих
HTTPD tasks/lwIP на C3. ESP32-сборки и аппаратная проверка остаются отложенными
по прямому указанию владельца. Windows/browser страница и её CSS/JS не менялись.

## Историческая проверка C3

Контракт и примеры: [HTTP API](readme_http_api_ru.md).
После включения SSE в компонент minimal пересобран и повторно проверен:
[результаты регрессии](readme_sse_library_verification_ru.md).

## Host

CMake/CTest, Zig 0.14.1 C++17, warnings-as-errors: 6/6 targets PASS.
- 139 pipeline checks: прежняя логика Chain/makeChain/FixedChain/access/challenge,
  новые пары Request/Response, response-only и отсутствие allocations на серии
  100 ответов с mock transport.
- 85 response/Basic checks: borrowed binary/NUL buffer identity, header lifetime,
  defaults/profile/individual override, пустой ответ/redirect, запрет повторной
  отправки, invalid/reserved headers, injection/NUL, лимиты/arena, transport errors,
  TextBuffer overflow/format/float, Basic full/truncated/padding/case и external abort.
- Прежние RequestBody, четыре percent decoder и оба production parameter backend
  Lookup/from_chars PASS. Go smoke unit tests PASS, новый acceptance client go vet PASS.

## C3: основной прогон

Повторно идентифицирована разрешённая ESP32-C3 rev0.4 на COM10, MAC сверен с ранее
проверенной платой. Device identity, IP, credentials и raw logs — только ignored local/.
Писался только app0 0x10000 в проверенной таблице разделов, размер меньше0x280000.
Hash verification PASS; flash backup, bootloader/partition/NVS/data writes не выполнялись.

На самом C3: 85 response/Basic unit checks PASS; 63 loopback HTTP/auth/body/WS PASS;
проверки production decoder/parser PASS. Response занимает **948 bytes** на C3;
на host x64 —1112 bytes. Это размер объекта с bounded header arena, не весь request
и не пик SDK/lwIP heap. HTTPD stack задан6144 bytes; compiler frame handle1168 B,
response send224 B, reject-drain352 B не являются суммарной глубиной стека.

С рабочей сети после получения IP выполнены:
- полный прежний Go smoke: HTTP/form/Basic/DELETE, partial POST, body4096/4097,
  raw/pointer/C-array/std::array/vector copies, binary/NUL0..4096, overflow и auth,
  WS text/binary0..4096, пять reconnect и HTTP после WS — PASS дважды;
- новая серия197 checks: обе сигнатуры endpoint, общие/per-route/индивидуальные
  заголовки и MIME, пустой204, redirect, TextBuffer, bounded request header read,
  response-only auth, запрет второго send, static/gzip,2 и4 параллельных клиента — PASS.

В первой версии сетевой прогон повторяемо получал timeout при POST4097. Отдельный
curl подтвердил отсутствие полученного ответа. Путь отказа закрывал соединение с
непрочитанным телом. Добавлен bounded drain только небольшого превышения: максимум
4352 bytes, deadline250 ms и отдельное ожидание socket read50 ms с восстановлением
его прежней настройки. После изменения прежний smoke прошёл полностью дважды.
Это не перенос streaming upload и не обещание graceful ответа на произвольный
гигантский/медленный upload; такой запрос всё ещё может закончиться закрытием/reset.
Причинную связь TCP reset с поведением рабочей сети пакетным capture не доказывали.

## Сборки и миграция

Minimal/C3 (Lookup), showcase/C3 и compile_check/C3 — build PASS.
Showcase только собран: его UI/CBOR/RGB сценарии на железе этой задачей не проверялись.
C3_std/from_chars сборка и финальное восстановление firmware отмечаются ниже после
завершения совместного аппаратного этапа. Другие SoC не собирались.

SDK: PlatformIO6.1.18, espressif32 6.10.0, ESP-IDF5.4.0, GCC14.2.0, C++17, -j2.
Synchronous Response не выделяет память под собственную оболочку/headers. Request
body/vector/parameter maps, HTTPD, lwIP и примеры могут выделять heap; zero-heap для
всего firmware не заявляется. Передаваемые body bytes заимствованы до конца send.

## Совместная интеграция и ограничения

HTTP/API-проверки и аппаратный этап соседнего чата идут последовательно на одной
плате. Contract/bridge использует resolveHeaders/claimExternal, не generic async HTTP.
Итог совместного этапа фиксируется в [отчёте интеграции](readme_sse_integration_verification_ru.md).
Нагрузочные проверки здесь короткие, не многодневный soak и не TLS/защищённый WS.
Commit/push не выполнялись.

## Финальный прогон обоих decoder profiles

После совместных проверок на той же C3 прошиты и проверены новые minimal/Lookup и
minimal/from_chars. У каждого —85 response/Basic,63 loopback и полный прежний LAN
smoke PASS, затем **198 новых HTTP checks** (включая resource snapshot и2/4 клиента).
Активный backend проверен по X-Url-Decoder на реальном /health. Сборка стандартного
варианта не подменяет аппаратную проверку: он действительно был отдельно прошит.

Снимки после серии198 checks, bytes:

| Profile | internal free heap | minimum heap | largest block | HTTP task minimum free stack |
|---|---:|---:|---:|---:|
| Lookup |198544|177508|114688|2124|
| from_chars |198540|177528|114688|2044|

В этом C3-порту StackType_t=uint8_t, поэтому high-water значение здесь в bytes.
Это короткие resource snapshots, не доказательство отсутствия утечек на многодневном
soak. Запас стека измерен реально, не получен суммированием compiler frames.

Firmware SHA-256:
- Lookup: `57f0ec531b880f91d19ff99825d10becd3ae6f4af874ab926a7c1c1b54ad817a`.
- from_chars: `3fd70e3bdbb0dea0fe657f5ddd03ffd3ee25a17f393b7744af42f1c0f5204afa`.

Размеры final minimal/Lookup: linker flash902666 B, static RAM32632 B; from_chars:
903790/32632 B. Эти сборки включают property/unit fixtures, включая static Capture
для тестов, и не являются размером одной HTTP-библиотеки.

Согласование соседнего adapter-чата завершено:708 host bridge checks, C3/LAN2551,
stop74, настоящий browser EventSource3 проверки; итог86 begin/86 complete,
active=0/pending=0. После последующего прямого поручения владельца SSE явно перенесён в src/sse и включён в библиотечный source list. tools/sse содержит только тесты и стенды; публичный вход — sse.h / NaWServer::sse::EventSource. Normal HTTP API остаётся синхронным. В конце основным чатом
восстановлен minimal/Lookup; адрес/состояние оставленной платы сообщены владельцу.

Source fingerprints и machine-readable сводка: tools/http_api_smoke/results/verification.json.
Raw evidence: ignored local/http-api-*; исходники/числовые результаты остальных
чатов сохранены. Git diff --check PASS, исходный TPMS не изменялся.

Последующее подтверждение в основном HTTP-чате: после переноса SSE в библиотеку
общий CTest повторно прошёл8/8, /health оставленной C3 вернул200 и X-Url-Decoder
lookup. Включение исходников SSE в компонент не запускает service автоматически;
на оставленной minimal-прошивке SSE service не запущен. Source fingerprints прежнего
HTTP-прогона остаются историческими; новый SSE library-прогон ведётся его отдельным отчётом.

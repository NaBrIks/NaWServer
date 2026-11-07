# Исследование общего HTTP/WS-движка поверх TCP

Дата: 2026-10-24. Исследование исходников и официальных источников; код не изменён.
Исходный просмотренный HEAD: `dde5c430a4cd6167bf7e6e6eaa849fd64c4d254f`.
В начале исследования в рабочем дереве были чужие изменения стиля/formatter и
генерируемого showcase asset; они не являются частью исследования. К завершению
HEAD стал `63ee25c55a332ce8078df625da194c9f48cebaa1` (обновление showcase assets).
Diff между этими HEAD для src/include/tests/host и корневого CMakeLists.txt пуст.
Номера строк ниже относятся к просмотренному
снимку; при продолжении сверять также имена классов и функций.

Решение владельца: [ADR](../adr/ADR_20261024_nawserver_shared-tcp-http-engine.md).
Практическая спецификация: [контракты](../plans/PLAN_20261024_nawserver_tcp-engine-contracts.md).
Порядок работ: [план и приёмка](../plans/PLAN_20261024_nawserver_tcp-engine-implementation.md).

## 1. Вывод

Направление целесообразно: уже есть независимый dispatcher и два реальных
платформенных потребителя. Следующая граница — TCP I/O под собственным HTTP/WS,
а не новая абстракция вокруг всей ОС. Native HTTPD остаётся самостоятельным backend.

Однако нынешний Windows backend — функциональная исходная точка, не готовое
переносимое ядро для МК. В одном файле смешаны Winsock, HTTP parser, WS codec,
поток исполнения, очереди, SHA-1/Base64, UTF-8 validation и SSE handoff. Простое
переименование сокетных функций переносит вместе с ним лишние копии, аллокации,
зависимости Windows и ограничения последовательного исполнения.

Наиболее важные риски — владение данными при частичной отправке, гонки закрытия,
неоднозначное HTTP framing и фактический суммарный memory budget. Цена виртуального
вызова или одного указателя вторична. Удаление HTTPD полезно как выбор состава
сборки, но уменьшение итоговой прошивки нужно измерить.

## 2. Что уже разделено правильно

| Граница | Проверенный код | Что сохраняем |
|---|---|---|
| Приложение / HTTP transport | [http_backend.h](../../include/NaWServer/http_backend.h) | HttpBackend lifecycle, HttpExchange callback lifetime |
| Общая обработка запроса | [http_server.cpp](../../src/http/http_server.cpp), `dispatchHttp`, строки 53–113 | Один порядок body → parameters → handler → pipeline → endpoint |
| Синхронный ответ | [response.h](../../include/NaWServer/response.h), `ResponseTransport` | Данные живут до возврата send; ошибка после начала отправки не создаёт второй ответ |
| Прикладной WS | [websocket_backend.h](../../include/NaWServer/websocket_backend.h) | Owned session token; borrowed receive bytes; copied asynchronous send |
| SSE | [sse_backend.h](../../include/NaWServer/sse_backend.h), [queued_adapter.hpp](../../include/NaWServer/detail/sse/queued_adapter.hpp) | reserve/activate/rollback, bounded queue, завершение владельцем |
| Native backend на дату отчёта | `src/backends/esp_idf.cpp`, `include/NaWServer/backends/esp_idf.h` (удалены в переносимой ревизии 03.11.2026) | SDK configuration/registration/lifecycle на дату сравнения |

`MemoryHttpBackend` получает уже разобранные запросы/сообщения. Это проверка
семантики приложения, а не HTTP/WS wire parser. Для нового движка нужен отдельный
scripted TCP transport, который подаёт реальные bytes и короткие операции.
Сохранить оба уровня тестирования; не переименовывать существующий memory backend
в TCP-тест и не считать его доказательством корректного framing.

## 3. Конкретные расходы и ловушки текущего кода

### 3.1 HTTP receive и request body

В [windows.cpp](../../src/backends/windows.cpp), `SocketExchange`:

- строки 261–263: отдельные `target_`, `pending_body_`, vector пар строк headers;
- `receiveHead()` около 344: `recv` сначала в stack chunk, затем append в string;
- около 363: остаток после заголовков копируется в `pending_body_`;
- около 432: каждый header материализуется в собственные строки;
- `readBody()` около 485: prefetched bytes снова копируются в destination.

В [dispatchHttp](../../src/http/http_server.cpp) строка 73 создаёт body vector
точного размера, затем данные читаются прямо в него. `setBody(std::move(body))`
передаёт владение, это не дополнительная копия payload. Нельзя объявить move
«лишним копированием» и ради него сломать владение.

Рекомендация: сначала убрать внутренние header strings из нового wire engine.
Затем дать TCP exchange безопасно заимствовать готовое тело из переиспользуемого
workspace. Это отдельный шаг dispatcher/RequestBody, с прежним ByteView/copyBodyTo
контрактом для приложения. Native HTTPD может оставаться на readBody fallback.
Не обещать streaming upload: сегодня обработчик видит полностью собранное тело,
а auth вызывается после чтения. Перенос auth до body меняет семантику.

### 3.2 Header convenience API и параметры

[Request::header](../../src/http/web_request.cpp) создаёт vector<char>, затем string.
`prepareParameters()` использует его для Content-Type. В
[form_parameters.cpp](../../src/http/form_parameters.cpp) декодирование создаёт
строки и materialized ParameterMap. Это известные расходы текущего API.

Первый перенос TCP не переписывает URL decoder, CBOR и parameter semantics.
Для нового parser нужны views; существующий `readHeader` намеренно копирует
в буфер вызывающего и сохраняет all-or-nothing/NUL contract. Новый внутренний
fast path не должен превращать возвращаемый `Request::header()` string в
неявно заимствованную память. Оптимизацию двойного выделения convenience API
вести отдельным измеряемым изменением после transport parity.

### 3.3 WebSocket: копии дороже абстракции

В [windows.cpp](../../src/backends/windows.cpp):

- `sendCopy()` около 630: payload помещается в vector очереди;
- `prepareWrite()` около 736: payload затем копируется в `active_output` вместе
  с frame header — вторая копия не нужна при двух последовательных spans;
- `appendReceived()` около 843: stack buffer 4096 → input vector;
- `readMessage()` около 1024: input → новый unmasked payload vector, затем erase
  с перемещением оставшихся bytes; фрагменты дополнительно объединяются;
- `WindowsWebSocketExchange` около 907 принимает target/headers по значению,
  а цикл около 1294 передаёт lvalues: handshake metadata копируется снова
  при последующих WS callbacks.

Сохранение handshake metadata один раз может быть оправдано: существующий
WebSocketExchange предоставляет head/readHeader и после открытия. Удалять эти
данные сразу после 101 без аудита всех HandlerBase-подклассов нельзя. Убирать
нужно повторную копию на каждый callback; snapshot на время сессии имеет явный
budget. «Views вместо strings» не освобождает память исходного snapshot.

Свой frame codec должен читать payload в конечный bounded receive buffer,
снимать mask на месте и поддерживать mask offset через короткие чтения.
Передавать queued payload без второй упаковки в полный frame vector.
Копия producer → owned queue остаётся необходимой для нынешнего sendCopy API.

### 3.4 Реальные границы памяти

Пример арифметики, не измеренный размер объекта:

- семь header buffers по 8192 B — 57344 B, то есть 56 KiB;
- четыре WS клиента × четыре pending × 4096 B — 65536 B payload, ещё без
  receive buffers, metadata, allocator overhead и текущей отправки;
- SSE [session.hpp](../../include/NaWServer/detail/sse/session.hpp) хранит в
  каждом из четырёх Slot три Frame queue и ещё Frame current. При frame_capacity
  768 B это уже 12288 B массивов payload, не считая полей/выравнивания;
- SSE `total_bytes=8192` — предел учёта queued bytes, а не размер `Sessions`;
  уменьшение runtime `clients` не уменьшает compile-time array из четырёх Slot.

Поэтому документировать два разных числа: зарезервированную память и допустимую
занятость. Нельзя переводить все vector в максимальные массивы на каждом сокете
и автоматически считать это экономией. Начальная конструкция использует один
HTTP workspace для последовательного HTTP и отдельные ограниченные WS/SSE pools.

SSE `Step` содержит копию до 256 B. Это сознательная граница: poll отпускает lock
перед сетевым I/O, а producer может менять queue. Удаление копии без pin/unpin
контракта создаст use-after-reuse. Оставить её до отдельного доказанного улучшения.

### 3.5 Исполнение: неблокирующий сокет не означает неблокирующий сервер

`WindowsHttpBackend::run()` около 1345 вызывает `receiveHead()`, затем dispatcher;
`SocketExchange::writeAll()` ждёт до deadline. В это время главный loop не вызывает
другие HTTP/WS callbacks. `O_NONBLOCK`/FIONBIO позволяет контролировать каждый
сетевой шаг, но не превращает синхронный endpoint в coroutine.

Есть фундаментальный выбор: заимствованный response buffer на стеке требует
закончить send до возврата; продолжение после callback требует owned storage
или отдельного completion API. Нельзя одновременно обещать произвольные
синхронные callbacks, отсутствие копий, один worker и нулевую задержку остальных.

Рекомендуемый первый вариант сохраняет последовательные HTTP/WS callbacks,
bounded waits/total deadlines и существующий отдельный SSE worker. При SSE
активации сокет получает ровно одного нового I/O-владельца. Это уже используемая
модель, а не новый worker pool. Если mixed-load измерения покажут неприемлемую
задержку нормальных клиентов, отдельно проектировать owned deferred transfers.
Не добавлять рекурсивный dispatch других callbacks из `Response::send`:
это сломает shared workspace, middleware context и предположения приложения.

### 3.6 Handshake, admission и ограничения API

Текущий Windows путь отправляет 101 до создания session/client и admission в
[WebSocketEndpoint::handleRequest](../../src/websocket/handler_ws.cpp).
При нехватке ресурсов он может закрыть уже upgraded socket. Для нового TCP
backend резервировать transport resources до первого байта 101; onConnect
вызывать только после полной успешной отправки handshake. Admission endpoint
потребует отдельной prepare/commit/rollback операции, а не вызова opening callback
«для проверки» с последующей отменой пользовательских побочных эффектов.

Это не означает готовую WS-аутентификацию. HTTP pipeline по-прежнему не защищает
WS. В IDF 6 есть официальный pre-handshake callback, но общий API, capability
checks и negative tests должны появиться до заявления authenticated WS.
На IDF 5.4 нельзя предположить наличие поля из IDF 6.

### 3.7 Почему выделения на соединение допустимы

`shared_ptr<WebSocketSession>` и owned `sse::Stream` уже защищают время жизни.
Механическое удаление shared_ptr ради одного control block опаснее его ограниченной
стоимости. Разрешить аллокации при start/admission с проверенным failure path.
Для горячего wire path использовать заранее выделенные буферы/пулы.
Владелец не требовал уничтожить всю динамическую память библиотеки.

## 4. Границы и альтернативы

| Вариант | Оценка |
|---|---|
| Перенести windows.cpp и заменить Winsock на macros | Отвергнуть: копии, crypto, Unicode, scheduler и framing останутся сцеплены |
| Общий HTTP/WS engine + узкий TCP interface + native HTTPD sibling | Рекомендуется: сохраняет API, позволяет wire tests без ОС и выбор прошивки |
| Raw lwIP API ради zero-copy | Не выбирать: вмешательство в TCP/IP context, другой lifetime и большая SDK-зависимость |
| Универсальные Network/Thread/Clock/Storage HAL | Не вводить: нет потребности в таком публичном framework |
| По потоку на соединение | Не выбирать по умолчанию: стеки по нескольку KiB и усложнение остановки |
| Сразу общий async HTTP API | Отложить: меняет срок жизни Response/Next и объём задачи |
| Обязательный TLS provider для SHA-1 WS | Не выбирать: handshake primitive не должен тащить весь TLS transport |

Принцип «один I/O-владелец на сокет» не требует одного потока на всю библиотеку.
HTTP/WS owner и SSE owner могут быть разными; передача владения должна быть
атомарной относительно close и завершаться до повторного использования slot.

### 4.1 Готовые parser/Arduino-библиотеки

«Свой HTTP backend» не обязывает заново писать каждый parser. Рассмотрены также
первичные описания [picohttpparser](https://github.com/h2o/picohttpparser) и
[llhttp](https://github.com/nodejs/llhttp). Первый не выделяет память и возвращает
указатели в переданный input buffer — это совместимо с выбранным владением.
Второй использует generated state machine и callbacks. Их размер на C3 здесь
не измерялся; утверждать, что любой из них «слишком большой», оснований нет.

Для первого этапа выбран extraction текущего ограниченного parser с сохранением
его policy/tests: он уже обслуживает действующий Windows backend. Это уменьшает
объём одновременных изменений, но оставляет ответственность за parser у проекта.
При расширении HTTP profile сначала сравнить с pinned picohttpparser/llhttp на
нашем corpus и по map/RAM, а не бесконечно расширять handwritten grammar.
Подмена parser — отдельное изменение с provenance, license notices и differential
tests; никаких загружаемых «latest» зависимостей в воспроизводимой сборке.

[AsyncTCP](https://github.com/ESP32Async/AsyncTCP) — реальный пример asynchronous
TCP-библиотеки ESP32, на которой основан ESPAsyncWebServer. Само наличие такой
библиотеки не доказывает переносимость её execution/lifetime модели на наш
синхронный Response или Windows/Linux. Её README обсуждает task stack и очередь:
слово async само по себе не означает отсутствие памяти/worker расходов.
Не предполагается, что владелец ранее видел именно эту библиотеку.

## 5. IDF 5 и 6

Опорная существующая конфигурация проекта — IDF 5.4.0. Для новой ветки матрицы
выбран опубликованный [IDF 6.0.3](https://github.com/espressif/esp-idf/releases/tag/v6.0.3).
Это фиксированная тестовая версия, не утверждение «самая новая». Первоначальная
нижняя граница поддержки — 5.4.0; 5.0–5.3 и каждый будущий minor не объявляются
проверенными без своей записи. В проекте остаётся C++17.

Официальные [lwIP 5.4.3](https://docs.espressif.com/projects/esp-idf/en/v5.4.3/esp32c3/api-guides/lwip.html)
и [lwIP 6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c3/api-guides/lwip.html)
подтверждают BSD sockets как поддерживаемую границу; Netconn не является
официально поддерживаемым API приложения. `poll` у IDF реализован через select.
В IDF запрещено удалять task, пока она находится внутри select/poll. Закрытие
из другого task может ждать завершения текущих операций. Следствие для проекта:
останавливать owner кооперативно, ждать выхода из I/O, не уничтожать task извне.

[HTTPD 5.4](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c3/api-reference/protocols/esp_http_server.html)
создаёт собственный task, listener и control socket. Его lifecycle нельзя
подменять generic close. [HTTPD 6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c3/api-reference/protocols/esp_http_server.html#websocket-pre-handshake-callback)
документирует `ws_pre_handshake_cb` и соответствующий Kconfig. Новое поле не
использовать без проверки headers выбранной версии и конфигурации.

[Миграция libc](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c3/migration-guides/release-6.x/6.0/system.html)
меняет default Newlib на Picolibc. Сравнение размера IDF5/httpd и IDF6/TCP смешает
эффекты двух изменений; измерять пары backend внутри каждой версии SDK.
Форматирование TextBuffer/float и обычные библиотеки C++ проверять в обеих ветках.

[Миграция security](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c3/migration-guides/release-6.x/6.0/security.html)
описывает Mbed TLS 4/PSA и удаление legacy crypto API/headers. Нельзя переносить
Windows CryptoAPI вызовы в core или повсюду использовать старый mbedtls_sha1 API.
Для WS нужна одна закрытая функция handshake digest; её выбранный переносимый
вариант и проверка известного вектора описаны в контракте. Это не TLS-библиотека.

HTTPD-only и TCP-only должны иметь раздельные source/dependency lists.
Сейчас [CMakeLists.txt](../../CMakeLists.txt) всегда включает native facade,
httpd_response, native WS/SSE и `REQUIRES esp_http_server`. В частности,
`src/sse/adapter.cpp` — IDF HTTPD adapter, несмотря на общее имя файла.
Условно выключить один вызов `httpd_start` недостаточно.
Состав компонентов определяется зависимостями
[IDF build system](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c3/api-guides/build-system.html).

Документация 6.0.3 по прямому URL lwIP не была доступна через web reader;
для API изучены versioned guides 6.0 и официальный release 6.0.3. Это не заменяет
компиляцию против фактических headers 6.0.3. IDF 6 в этой задаче не собирался.

## 6. Основания сетевого контракта

[Winsock send](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-send)
может принять меньше запрошенных bytes; успешная отправка не доказывает delivery.
[Winsock select](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select)
имеет собственное представление fd_set и игнорирует nfds. Следствие: OS handles,
sets и error codes остаются в платформенном файле; core получает нормализованные
результаты и offset, не использует Windows SOCKET как int.

На Linux [send(2)](https://man7.org/linux/man-pages/man2/send.2.html) предоставляет
MSG_NOSIGNAL для подавления SIGPIPE на конкретном вызове. Использовать его в
Linux adapter, не менять обработчики сигналов всего процесса. После закрытия
descriptor может быть переиспользован; [close(2)](https://man7.org/linux/man-pages/man2/close.2.html)
объясняет опасность повторного close после ошибки. В engine нужен generation token,
а не один fd как идентификатор жизни соединения.

[RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3) определяет
HTTP framing; ошибки длины/CL+TE нельзя лечить эвристикой. Это основание строгой
таблицы отказов проекта и byte-stream tests. [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.3.2)
разделяет semantics HEAD и содержимое ответа; специальное поведение 204/304
нужно проверять на wire, а не только по возвращённому status.

[RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html#section-5) задаёт masking,
fragmentation и control frames. Существующий onMessage API требует целого bounded
сообщения; поэтому накопление fragmented message оправдано, а многократная
перепаковка полного payload — нет. [WHATWG EventSource](https://html.spec.whatwg.org/multipage/server-sent-events.html)
описывает UTF-8 event stream и reconnect/Last-Event-ID; transport reuse не означает
автоматический replay истории в сервере.

Ни одна из этих спецификаций не требует выбранных нами чисел buffers/queues,
числа tasks или конкретных C++ классов. Эти параметры — решения проекта,
обоснованные текущим API и ограничениями МК.

## 7. Linux рядом с ioBroker

Речь об отдельном executable на той же Linux-машине, а не об ioBroker adapter.
Исторические заметки указывают на отдельные источники истины инфраструктуры и
прикладных проектов; актуальные CPU/RAM/дистрибутив/свободные порты в этой задаче
не проверялись. Развёртывания или удалённых изменений не выполнялось.

Полнота проверки не означает максимальную нагрузку на рабочий домашний сервер.
На Windows и изолированном Linux выполнять fuzz, connection floods, forced OOM,
санитайзеры и stress. Рядом с ioBroker — bounded функциональные тесты и soak
с контролем ресурсов. Контейнер без лимитов не является достаточной изоляцией;
CPU/memory/pids/log quotas должны быть реально применены и прочитаны обратно.
`nice` не ограничивает память и не заменяет cgroup.

[systemd-run](https://www.freedesktop.org/software/systemd/man/latest/systemd-run.html)
позволяет transient service без постоянного автозапуска;
[resource-control](https://www.freedesktop.org/software/systemd/man/latest/systemd.resource-control.html)
описывает CPUQuota/MemoryMax/TasksMax. Доступность user delegation и конкретных
свойств зависит от установленной версии. Проверка локальных man/help и effective
cgroup обязательна до нагрузки. Точный runbook — в плане; сервер не менять
ради установки новой среды, если подходящей изоляции нет.

## 8. Проверки, выполненные при исследовании

Выполнена попытка пересборки существующего `local/http-host-build`:

1. В sandbox Zig завершился с `unable to find zig self exe path: Unexpected`.
2. Повтор с разрешённым запуском пересобрал test targets и общую библиотеку.
   Финальная линковка demo не завершилась: `failed to write output ... Permission denied`.
   Блокировка файлом/процессом — возможная причина, но текущий владелец блокировки
   не подтверждён. Процессы не завершались. Полная host-сборка не отмечается PASS.
3. CTest из build directory: **17/18 PASS**, `windows_sse` завершился
   `WINDOWS_SSE_FAIL line=282`, exit `0xc0000409`.
4. Один отдельный повтор `windows_sse`: **PASS**. Это признак нестабильного
   результата, а не доказательство исправления или гарантированного отсутствия бага.

Строка 282 [windows_sse_test.cpp](../../tests/windows_sse_test.cpp) проверяет
`events.active() == 0` сразу после EOF на клиенте. В
[QueuedAdapter::poll](../../src/sse/queued_adapter.cpp) сначала выполняется
`stream->finish()`, затем под lock `sessions_.completed(step)` уменьшает active.
EOF может наблюдаться между этими действиями. Это обоснованная гипотеза гонки
наблюдения теста, но план требует отдельно доказать eventual completion и
отсутствие утечки; запрещено просто удалить assert или добавить произвольный sleep.

Существующая версия ctest не применила `--test-dir` и сначала вывела
`No tests were found` из корня. Засчитан только последующий запуск из
`local/http-host-build`. Исторические отчёты 16/16 и прежняя C3-приёмка не
заменяют результат этой задачи. Browser, Linux, C3 и будущий TCP engine здесь
не проверялись; их полная программа зафиксирована отдельно.

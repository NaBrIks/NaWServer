# Контракты общего TCP HTTP backend

Дата: 2026-10-24. Обновлено: 2026-10-25. Статус: спецификация реализации;
P0–P5 выполнены, общие TCP HTTP/WS/SSE и memory pool подключены на Windows/Linux.
MCU TCP runtime ещё предстоит. Результаты:
[текущий отчёт](../readme_tcp_engine_verification_ru.md).
Принятое направление — [ADR](../adr/ADR_20261024_nawserver_shared-tcp-http-engine.md).
Причины и найденные расходы — [исследование](../reviews/2026-10-24_shared-tcp-http-engine.md).
Этапы и обязательные проверки — [план](PLAN_20261024_nawserver_tcp-engine-implementation.md).

Этот документ выбирает одну начальную конструкцию. Не смешивать её с необязательными
улучшениями из последнего раздела. Числа стартового профиля ниже — тестовые
инженерные значения, не обещание размеров или быстродействия.

## 1. Слои и зависимости

```text
Приложение: network init, routes, middleware, endpoints, producers
                         |
              HttpServer / dispatchHttp
                         |
                  HttpBackend boundary
             /                           \
 EspIdfHttpBackend                 TcpHttpBackend
 HTTPD request/response            HTTP parser/writer + WS codec
 native WS + Idf SSE               TCP sessions + TCP SSE binding
             |                           |
      esp_http_server              TcpTransport
             |                  /       |       \
       SDK TCP/lwIP          Winsock    Linux    IDF BSD sockets
```

Существующий `Server` из server.h — native IDF facade, а `HttpServer` — общий.
В TCP-only примере использовать `HttpServer`; не маскировать IDF-specific
native_handle/espIdfRequest псевдорезультатами. Примеры составляют конкретные
backend объекты статически. Constructor ничего не запускает.

Названия новых типов/путей ниже предлагаются как единая схема, которую следует
применять последовательно:

| Путь | Ответственность |
|---|---|
| `include/NaWServer/backends/tcp.h` | TcpHttpBackend, явные storage/runtime options; без OS headers |
| `include/NaWServer/backends/tcp_transport.h` | Малый контракт transport для нескольких сокетов |
| `include/NaWServer/backends/windows_tcp.h`, `linux_tcp.h`, `esp_idf_tcp.h` | Конкретная транспортная композиция; SDK допустим только в собственном platform header |
| `src/http/wire/` | Bounded HTTP parser, header index, response writer; без sockets/tasks |
| `src/websocket/wire/` | Handshake и incremental frame codec; без Winsock/IDF |
| `src/backends/tcp/` | Connection lifecycle, HttpExchange, WS queue/session, SSE stream binding |
| `src/backends/tcp/platform/` | OS sockets, wait sets, worker start/join, native error details |
| `src/sse/`, `include/NaWServer/detail/sse/` | Существующие encoder/Sessions/bridge; единственная реализация SSE |
| `tests/tcp_*`, `tests/http_wire_*`, `tests/websocket_wire_*` | Scripted byte transport и wire corpus |

Не создавать public abstract Task/Mutex/FileSystem/NetworkManager. Monotonic time
передаётся engine как число milliseconds; worker glue остаётся private. Runtime
пользуется одним заимствованным transport object, а не heap-фабрикой на каждый socket.
Статическое связывание/virtual dispatch на границе допустимо; шаблонные копии
всего движка для каждой ОС не нужны.

## 2. TCP contract

### 2.1 Данные и операции

Платформенные handles не выходят в приложение. Для runtime допустим opaque
`TcpHandle`, способный вместить `uintptr_t`; это не идентификатор сессии для
producer. Lifetime token — пара `slot + uint64 generation`, проверяемая перед
каждым queued действием. При исчерпании generation slot больше не переиспользовать.
Handle со значением 0 не считать автоматически невалидным. Реализуемый P1 тип
`tcp::Handle` содержит отдельный valid flag; копии handle заимствованы, close
выполняет единственный владелец. `close(Handle&)` делает одну native попытку и
инвалидирует handle только если `CloseResult::released` истинно. Ошибка с released
не повторяется: Linux descriptor уже мог быть переиспользован. При retained
failure сохранять slot и зависимости; неизвестный исход не разрешает слепой retry.
Подробная platform policy проверяется отдельно для Winsock, Linux и lwIP.
P2 уточнил startup/accept rollback: optional `cleanup` передаёт предыдущий
CloseResult. nullopt означает, что close ещё не вызывался; известный retained
WouldBlock допускает отдельную явную попытку cleanup, ambiguous Fatal — нет.

Transport допускает одновременные операции разных owners над разными handles.
Wait sets и scratch принадлежат вызову/worker, не общему mutable singleton.
Операции одного handle выполняет только его текущий owner. Обновление общей
таблицы registrations защищается короткой синхронизацией без network I/O под lock.

Transport предоставляет только следующие операции:

| Операция | Контракт |
|---|---|
| openListener(address, port, backlog) | Явный numeric bind address, port=0 допустим; возвращает actual port; listener nonblocking |
| accept(listener) | Не ждёт; принятый socket переведён в nonblocking до публикации handle |
| readSome(handle, mutable bytes) | Один bounded syscall; не изменяет данные за возвращённым размером |
| writeSome(handle, const bytes) | Один bounded syscall; не сохраняет pointer после возврата |
| wait(interests, timeout) | Ограниченное ожидание read/write/error; caller-owned массив результатов |
| shutdownWrite(handle) | Запрет дальнейшей отправки, если допустимо для данного состояния |
| close(handle) | Одна попытка освобождения native handle владельцем; повторное использование token запрещено |

`readSome/writeSome` требуют непустой span; пустой logical payload обрабатывает
вызывающий без syscall. Результаты: `Progress(n>0)`, `WouldBlock`, `Interrupted`,
`PeerClosed`, `Fatal(native_code)`. Только Progress несёт bytes. `recv==0` —
PeerClosed; `send==0` при непустом span — Fatal/no-progress, не успешная передача.
`n > requested` — ошибка adapter, детектируемая тестом. EOF во время неполного
HTTP body/frame не становится успешным пустым сообщением.

EOF чтения означает закрытую входящую половину TCP. Если полный HTTP request
уже получен, ответ ещё можно отправить; не превращать нормальный client
shutdown-write в безусловный запрет response. Send failure определяется отдельно.

Timeout — результат ожидания/engine deadline, а не алиас WouldBlock. При EINTR
engine повторно проверяет stop/deadline; бесконечный внутренний retry запрещён.
native error сохраняется для диагностики, прикладной API получает HttpError/
SendResult. Readiness является подсказкой: следующий syscall вправе вернуть
WouldBlock. Адаптер ограничивает длину syscall своим типом (`int` в Winsock),
не сужает size_t без проверки.

Конкретизация P1: `wait` получает два caller-owned массива одинаковой длины;
Ready entries соответствуют input interests один к одному. Дубликаты handles
допустимы, ready_count считает entries с любым событием, а не сумму memberships
native sets. Все outputs обнуляются также при timeout/interrupt/error. Пустой
wait и пустой интерес отклоняются как InvalidArgument. Native handle cardinality
проверяется до FD_SET; numeric значение Windows SOCKET не ограничивается FD_SETSIZE.
Platform startup/stop остаётся конкретным API transport; конструктор инертен.
Stop не освобождает platform runtime, пока остаются owned sockets.

### 2.2 Платформенные различия

| Windows | Linux | IDF 5/6 |
|---|---|---|
| SOCKET/INVALID_SOCKET | int, -1 | BSD sockets из lwIP |
| WSAGetLastError, WSAEWOULDBLOCK | errno, EAGAIN/EWOULDBLOCK/EINTR | errno; проверять headers конкретного SDK |
| ioctlsocket(FIONBIO) | fcntl(O_NONBLOCK), preserve existing flags | fcntl поддерживаемых flags |
| closesocket, SD_SEND | close, SHUT_WR | close/shutdown |
| select, count bound fd_set | poll, bounded array; не требуется fd<FD_SETSIZE | select; проверять доступный диапазон descriptor до FD_SET |
| WSAStartup/WSACleanup balanced per adapter lifecycle | MSG_NOSIGNAL в send; не менять process-wide SIGPIPE | default socket API, без raw/netconn |

Основания: [Microsoft send](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-send),
[Microsoft select](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select),
[Linux send](https://man7.org/linux/man-pages/man2/send.2.html),
[IDF lwIP](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c3/api-guides/lwip.html).
Общие структуры не содержат fd_set, sockaddr, timeval, errno или SDK enums.
Начальный listener — IPv4; адрес задаётся явно. IPv6 — последующий отдельный этап.
Для Linux binding использовать обычный эксклюзивный bind без SO_REUSEPORT;
SO_REUSEADDR только для restart-сценария, с тестом занятого порта. Windows сохранить
SO_EXCLUSIVEADDRUSE и проверять результат setsockopt.

## 3. Runtime и ownership

### 3.1 Начальная модель исполнения

Один HTTP/WS worker на backend; callbacks последовательны, вложенный dispatch
запрещён. Один отдельный SSE worker только если явно запущен EventSource с TCP
SSE adapter. Это сохраняет текущую проверенную границу двух owners. Нет task на
каждый клиент, фонового file-worker pool или автоматического reentrant pump.

HTTP обрабатывается последовательно, включая чтение headers/body. Используется
один shared HTTP workspace. Остальные соединения находятся в bounded OS backlog
либо уже принадлежат WS/SSE. Backlog не считать точным числом активных sockets;
`max_connections` учитывает принятые HTTP+WS+SSE. Предел ресурсов SDK/ОС отдельный.

Синхронный send ограничивает время занятости worker сетевым deadline, но пока он
идёт, HTTP/WS callbacks ждут. SSE обслуживает свой owner. Это осознанный предел
первой реализации. После deadline система должна восстановить HTTP/WS progress;
требование «любой другой запрос всегда отвечает за 50 ms» не заявляется.
Произвольный блокирующий код приложения/endpoint нельзя прервать transport timeout.

Рабочие циклы делают bounded syscalls, проверяют stop между шагами и не крутятся
на WouldBlock. Waiting ceiling начального профиля — 20 ms. На IDF округлять
положительные интервалы к минимум одному tick и учитывать округление в тестах.
Не реализовывать sleep(0) loop. Новая очередь становится видимой worker не позже
следующего ограниченного wait; отдельный wakeup socket на первом этапе не нужен.

### 3.2 Состояния соединения

```text
Free → Accepted → ReadingHead → ReadingBody → Dispatching → Sending → Closing → Free
                                    |
                       (после complete head, zero body)
                             UpgradeReserved → Switching → WebSocket → Closing

Dispatching → SseReserved → SseActive → SseFinishing → Free
                   └─ rollback → Dispatching
```

HTTP owner закрывает обычный/WS socket. SSE reserve ещё не передаёт право I/O:
до activate никаких bytes SSE и close со стороны SSE owner. Activate фиксирует
передачу, после чего HTTP worker не читает/пишет/закрывает этот handle. Finish
выполняется SSE owner; release registration и slot происходят ровно один раз.
Приложение видит только token/Stream, не fd.

При rollback возвращается исходное владение; при ошибке после отправленного
HTTP/SSE/WS префикса нельзя послать второй HTTP error. Только ограниченное
завершение/close. Данные, прочитанные за границу HTTP head, не терять: обычное
тело получает их как prefix; WS parser получает bytes первого frame после 101.

### 3.3 Остановка

1. Приложение останавливает producers.
2. EventSource.stop(timeout) запрещает новые публикации и завершает SSE streams.
3. Только после успешного SSE stop вызывается server.end(). При активных SSE
   возвращать InvalidState, сохраняя рабочий backend для корректного drain/retry.
4. HTTP/WS stop прекращает admission, отмечает stop, даёт owner выйти из wait/send,
   закрыть свои sockets и завершить callbacks; затем join.
5. Ошибка cleanup сохраняет зависимости/handles и требует повторного end;
   не считать backend остановленным только потому, что stop был вызван.

Для нового TCP backend задать `stop_timeout_ms=5000` в runtime options.
Host stop сначала ждёт флаг worker_done с этим пределом и только затем делает
join; при истечении возвращает CleanupRequired и оставляет thread/зависимости
живыми. IDF glue аналогично ждёт owner completion, не удаляет task извне.
Destructor не является механизмом аварийного освобождения зависимостей при
неуспешном stop: приложение обязано дождаться success. Нативный HTTPD stop
имеет собственный SDK contract и не получает фиктивную такую же гарантию времени.

Чужой поток не делает close одновременно с I/O; stop request не переиспользует
fd и не удаляет task. Успешный stop гарантирует отсутствие дальнейших callbacks.
Stop из callback отклоняется. В IDF HTTPD пути использовать его собственный
stop/drain contract, не подключать к нему generic TCP close.

## 4. HTTP wire profile

Базовый профиль сохраняет действующий Windows subset: HTTP/1.0/1.1,
Content-Length, один HTTP request на соединение, Connection: close.
Keep-alive/pipelining, chunked upload, Expect/100-continue, proxy/CONNECT и TLS
в первый этап не входят. Это ограниченный профиль, не заявление полной
реализации всех обязательств произвольного HTTP/1.1 сервера.

Parser работает с octets, не Unicode strings. Накапливает head в неподвижном
buffer и строит bounded индекс offsets/lengths. Не изменяет raw URI/body ради
normalization. Header matching case-insensitive ASCII, route/path semantics
берутся из существующего dispatcher. Имена/значения проверяются текущим
`header_validation.h`; URL decoding остаётся существующим.

Ниже — выбранная политика проекта; нормативное основание framing:
[RFC 9112 §§2, 5, 6](https://www.rfc-editor.org/rfc/rfc9112.html).

| Вход / условие | Результат первого профиля |
|---|---|
| Корректные HTTP/1.0/1.1, target начинается `/` | Диспетчеризация после полного bounded body |
| Неподдержанная версия HTTP | 505 + close |
| Синтаксически допустимый неизвестный method | HttpMethod::Unknown; обычный поиск route, обычно 404 |
| Невалидный method token, request line, bare LF, obs-fold, whitespace перед colon, NUL/control | 400 + close |
| HTTP/1.1 без Host; duplicate Host; пустой Host | 400 + close |
| Target с `#`, пробелом/control, absolute/authority/asterisk form | 400 + close (ограниченный origin-form профиль) |
| Target больше max_target_size | 414 + close |
| Head больше max_header_bytes или слишком много headers | 431 + close |
| Любой duplicate Content-Length, list value, знак, не decimal, overflow | 400 + close |
| CL вместе с Transfer-Encoding | 400 + close |
| Любой Transfer-Encoding без CL | 501 + close; никаких попыток прочитать chunked как plain body |
| Любой Expect | 417 + close |
| Body больше limit | 400 + close: намеренно сохраняется текущий прикладной контракт |
| Неполный body до deadline | 408 + close, если ещё можно отправить ответ |
| EOF в неполном body | 400/close; транспорт может уже не позволить доставить response |
| Bytes после declared body | Второй запрос не исполнять; завершить первое соединение |

Предел head включает request line и завершающий CRLFCRLF, но не coalesced body.
Ровно limit bytes с последним delimiter допустимы. Алгоритм ищет delimiter только
в ещё не просмотренной части с overlap 3 bytes; не сканирует весь buffer заново
после каждого принятого байта. Использовать subtraction checks до сложения размеров.
На ошибке не продолжать parsing соседнего запроса из того же соединения.

Остальные duplicate headers сохраняются в порядке получения; `readHeader`
возвращает первое значение, как текущий backend. Не объединять их автоматически
через запятую и не вводить новую политику auth/parameters скрыто в parser.

Private parser interface: runtime читает в свободный хвост head storage и вызывает
`advance(used_bytes)`. Parser не принимает владение внешними bytes и не делает
свой append. Результат — NeedMore, Complete(head_bytes, parsed metadata) или
Reject(status). `used_bytes` монотонен до reset, metadata — offsets в том же
storage. Reset разрешён только после окончания всех заимствованных views.
Parser не делает syscalls, не вызывает endpoints и не выбирает соединение.

Response writer сначала проверяет headers/length/status и только затем делает I/O.
Сохраняет bounded Response header storage. Собирает небольшой status/length prefix
в локальном буфере; отправляет последовательность spans без полного response string.
Header/body spans заимствованы до конца синхронного send. Partial writes продвигают
offset; при error возвращают TransportError, никакой повторной отправки с начала.

HEAD не отправляет body bytes; Content-Length отражает размер представления.
204/304 в текущем API требуют пустой body и не получают искусственный payload.
Протокольный 101 идёт отдельным handshake writer, не через публичный Response,
который принимает 200–599. Основание semantics:
[RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html).

Финал обычного ответа: shutdown write и небольшой bounded drain как в текущем
backend (не более 4096 B / 50 ms), затем close. Для маленького превышения body
сохранить существующий отдельный drain до max_body+256 B / 250 ms перед ошибкой.
Все дополнительные waits входят в общий измеряемый бюджет; при stop не drain.
Не обещать доставленный error response произвольно большому/оборванному upload.

## 5. Storage и copies

### 5.1 Стартовый профиль

| Объект / лимит | Начальное значение / правило |
|---|---|
| Принятые соединения | 7 суммарно HTTP+WS+SSE; SDK socket capacity больше с учётом служб |
| HTTP workspace | Один на сериализованный HTTP worker |
| Head / индекс / target | 8192 B head, 32 entries, 1024 B target limit; target — view внутри head |
| Body | 4096 B, отдельный переиспользуемый buffer одного HTTP workspace |
| WS clients / message | 4, max_message_size 4096 B |
| WS outgoing pool | 4 payload blocks по 4096 B; глобально 16384 B, queued+inflight вместе |
| WS pending per client | До 4, дополнительно ограничено глобальным pool |
| WS receive | Один bounded message buffer 4096 B на принятую WS session + frame header/control scratch |
| WS handshake metadata | Один компактный snapshot фактически использованных bytes на session, не 8192 B безусловно; max 8192 B |
| SSE | Текущие encoder/Sessions limits; учитывать reserved arrays отдельно от queued_bytes |
| HTTP exchange deadline | 5000 ms, от начала обработки accepted HTTP до конца response |
| Socket no-progress bound | 1000 ms внутри общего deadline; прогресс не продлевает total deadline |
| WS outgoing deadline | 1000 ms от взятия сообщения в active output; queue age измеряется отдельно |
| SSE deadlines | Текущие 2000 ms stall / 5000 ms event transfer / 15000 ms heartbeat |

WS pool — один на TcpHttpBackend, не отдельные 16 KiB на каждого endpoint.
Более крупные message limits требуют явно большего storage profile и проверенного
start; нельзя молча обрезать сообщение. Compile-time capacities и runtime limits
проверяются до listener start. Выделять динамическое backing storage один раз на
start либо заимствовать caller storage; первый вариант допустим, повторных growth
на traffic path быть не должно. Внутри выбранного режима не иметь скрытого fallback
на heap при полном pool.

Эти значения не обязаны сразу стать новыми defaults всего публичного API.
Это именованный acceptance profile для нового backend. Existing HTTPD limits
остаются SDK-owned и отдельно записываются. Не выдавать разные реальные limits
за одинаковые только из-за одинакового имени поля HttpLimits.

### 5.2 Карта владения

| Данные | Владелец и срок | Разрешённые копии |
|---|---|---|
| HTTP head + target + header index | HTTP workspace, до завершения callback/send/handoff | recv прямо в head; views без копии каждого поля |
| Body TCP запроса | HTTP workspace, тот же срок | Coalesced prefix один раз в body buffer, остальные recv прямо в body |
| Body HTTPD/memory | Прежний RequestBody owner до отдельной миграции | Текущий readBody/move contract |
| `readHeader` destination | Вызывающий | Явная ограниченная copy API |
| Синхронный ответ | Приложение / стек callback | Payload copy внутри NaWServer не требуется |
| Queued WS payload | Общий TX pool до полного send/abort | Одна копия producer → pool |
| WS incoming message | Session receive buffer, до конца onMessage | In-place unmask; фрагменты сразу в конечные offsets |
| Handshake metadata WS | Session snapshot до disconnect | Одна копия использованных bytes, никаких повторных copies на onMessage |
| SSE event | Encoder Frame → Sessions | Существующая bounded ownership copy, Step snapshot до 256 B оправдан lock boundary |

Предложенное расширение для второго HTTP storage этапа: `HttpExchange::preparedBody()`
возвращает `optional<ByteView>` со значением только когда полный body уже находится
в стабильной памяти exchange. `nullopt` означает использовать существующий readBody
путь; пустой ByteView со значением означает подготовленное тело нулевой длины.
Dispatcher проверяет size==head.content_length и limits, затем заимствует его
через private `Request::setBodyView`. Ни Request, ни этот view не переживают callback.
Это новый capability path, не новая семантика тела и не streaming API.

`RequestBody` хранит либо owned vector, либо borrowed view; owned view вычисляется
из vector при обращении. Не хранить pointer в старый vector после move. copyBodyTo
во всех вариантах имеет прежние результаты, binary/NUL и all-or-nothing semantics.
Не применять preparedBody без проверки lifetime и теста повторного workspace reuse.

Отказы новых явных storage/transport allocations при start/admission должны
возвращать отказ и освобождать всё уже созданное.
Код core не требует включения C++ exceptions на IDF. Не копировать безусловные
try/catch из Windows в сборку с `-fno-exceptions`; сделать nothrow allocations и
фиксированные контейнеры в новом горячем пути. Аллокации существующих parameter
maps и callback objects остаются отмеченными в ledger до отдельного этапа.

`std::shared_ptr` control block и стандартные owning containers также выделяют
память; одного `new(std::nothrow)` для payload недостаточно для полной защиты
от OOM. Сохраняемые существующие session/Stream tokens могут аллоцировать при
admission; не обещать recoverable global heap exhaustion при выключенных exceptions.
Forced failure tests нового pool не доказывают это свойство. Первоначальная
приёмка требует ограниченного числа таких объектов и достаточного измеренного
heap reserve. Полный recoverable-OOM контракт всего API потребует отдельного
решения о bounded token storage; не внедрять самодельный shared_ptr/allocator
framework скрыто в этот перенос.

## 6. WebSocket codec и очередь

Сохраняются text/binary messages, fragmentation и interleaved control frames.
Parser инкрементальный: frame header до 14 B, mask offset, message size, fragmented
state; проверка объявленной длины до чтения/выделения payload. На входе клиента
mask обязателен, server output немаскирован. Control payload до 125 B и FIN;
невалидные reserved bits/opcodes/continuation/length → protocol close 1002,
невалидный UTF-8 → 1007, превышение message limit → 1009.
Close/status rules реализовать по [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html),
а не по случайному набору browser примеров. Unknown extensions не согласовывать;
permessage-deflate отсутствует. Начальный профиль не выбирает subprotocol.

Handshake profile: GET, HTTP/1.1, zero body, Upgrade/Connection tokens, version 13,
строгий canonical key, единственный экземпляр каждого управляющего поля.
Сначала все validation и allocation/reservation, потом полный 101, потом commit
и onConnect. Ошибка до 101 может вернуть HTTP error; после частичного 101 — close.
Нельзя вызывать onConnect до commit или повторно при retry partial write.

WS handshake helper — закрытая переносимая функция `makeWebSocketAccept`, а не
публичный crypto provider. Рекомендуется небольшой общий implementation алгоритма
SHA-1 по [RFC 3174](https://www.rfc-editor.org/rfc/rfc3174.html) и Base64 по
[RFC 4648](https://www.rfc-editor.org/rfc/rfc4648.html), только для protocol handshake.
Вход digest — 60 ASCII bytes (24-character key + 36-character GUID), выход 20 B,
Base64 accept 28 chars. Не использовать SHA-1 для password/signature/auth.
Реализовать по алгоритму, не переносить без provenance сторонний исходник;
LICENSE проекта сохранить. Проверять RFC6455 sample key/accept и независимый
Go `crypto/sha1`/`encoding/base64` oracle на наборе ключей. OS crypto libraries
перестают быть обязательной зависимостью общего WS движка.

UTF-8 проверка уже есть в SSE encoder. При WS extraction выделить её алгоритм
в private shared helper с тестами [RFC 3629](https://www.rfc-editor.org/rfc/rfc3629.html),
сохранив SSE contract. Не тащить весь SSE runtime в WS-only binary и не использовать
MultiByteToWideChar на других платформах. Это необходимое выделение зависимости,
не переписывание URL/CBOR decoders. Проверять полное собранное text message;
граница frame может проходить внутри Unicode sequence.

Queue contract:

- `Queued`: producer payload уже owned; slot считается занятым и при partial send.
- Нельзя держать queue mutex во время socket wait/send или user callback.
- Producer использует try-lock; новый результат `Busy` отделён от сетевого отказа.
- Индивидуальный лимит → `QueueFull` и request-close данной session согласно ADR.
- Глобальный pool exhausted → новый `GlobalFull`, без обвинения/закрытия конкретного
  клиента. Новые enum values и callers/tests обновлять одним отдельным этапом.
- Ошибка инвалидного token → Closed; слишком большое сообщение → TooLarge без copy.
- Active output хранит короткий header и ссылку на owned payload block с offsets.
  Не вставлять control frame в середину частично переданного frame. Control очередь
  отдельная и bounded (начальный budget 512 B), close имеет резерв внутри неё.
- После timeout partial frame не продолжать другим frame: закрыть socket и освободить
  active/queued blocks ровно один раз. Подмена payload при повторном использовании
  pool запрещена до завершения текущего send.

Реализация P5 уточнила TCP cleanup после полного WS Close: не более одного
shutdownWrite, затем incremental raw discard в прежний RX (≤256 B/readiness,
≤4096 B всего, absolute50ms). Deadline проверяется перед I/O после wait;
нет decoder/callbacks после Close. Это устраняет Linux RST при непрочитанной маске
раннего rejected frame. Фактическая reclamation зависит от owner scheduling;
flood сверх бюджета/reset не имеет гарантии доставки close code. Shutdown error
или partial-TX failure приводит к abort, без скрытого retry или второго frame.

Endpoint admission до 101 потребует внутренней Reservation с rollback. В ходе
reserve создаются client/session/контейнерная запись, но пользовательские callbacks
ещё не вызываются. Commit не аллоцирует. Native HTTPD сохраняет свой admission path
и отдельно документированные возможности; отсутствие pre-handshake hook на
конкретном SDK нельзя скрывать. Authenticated WS остаётся unsupported до отдельного
контракта policy и negative tests, даже если native SDK умеет hook.

## 7. SSE binding

Переиспользовать `QueuedAdapter`/Sessions/encoder/Response bridge. Новый TcpStream
оборачивает connection lease, а не сохраняет HttpExchange/Request/Response/Next.
Stream::write переводит TCP Progress/WouldBlock/fatal в существующий SSE результат.
Activate/rollback/finish сохраняют контракт раздела 3. Не копировать SSE encoder
в platform directories и не заменять собственную queue ещё одной TCP output queue.

Worker polling и clock принадлежат TCP SSE adapter. На host — private thread glue,
на IDF — private FreeRTOS glue. EventSource.start() остаётся явным; связывание
backend/HTTP owner проверяется при reserve. Нельзя использовать native IdfAdapter
с TCP-only backend, поскольку он опирается на HTTPD async request.

SSE response остаётся close-delimited без Content-Length; не требует chunked
output и не разделяет connection с последующим HTTP request. После activate
обычный Response не отправляет ничего. Stop producers → EventSource.stop →
HttpServer.end, с проверкой всех результатов. EOF на клиенте и local completed
counter — разные моменты; тест ждёт completion с deadline.

## 8. Build contract для IDF 5/6

Предложить Kconfig choice `NAWSERVER_HTTP_BACKEND_IDF_HTTPD` /
`NAWSERVER_HTTP_BACKEND_TCP`, default HTTPD на первом этапе. Отдельные флаги WS/SSE
контролируют component source lists. Общий код компилируется один раз.
Конкретный набор runtime limits остаётся явным в приложении, не сотнями Kconfig.

| Профиль | Источники и зависимости |
|---|---|
| HTTPD-only | Shared dispatcher/API + esp_idf/httpd_response/web_server + native WS/SSE по флагам; esp_http_server |
| TCP-only IDF | Shared dispatcher/API + wire codecs + TCP runtime + lwIP adapter + TCP SSE при включении; explicit lwip/FreeRTOS/timer dependencies по используемым headers |
| Host TCP | Тот же wire/runtime, Winsock или Linux sources; без SDK stubs |
| Memory semantics | Существующий MemoryHttpBackend, без обязательного wire/runtime |
| Scripted TCP tests | Реальный wire/runtime c scripted I/O, fake monotonic clock; не SDK stubs |

`server.h` и `backends/esp_idf*.h` не становятся общими заголовками. В TCP-only
сборке они не требуются; попытка использовать выключенный backend должна давать
понятную compile/link-time ошибку, а не runtime fallback. Отдельный include-smoke
с общими headers собирается без esp_http_server include paths.

В IDF5 и IDF6 проверять HTTPD-only и TCP-only на C3, WS/SSE off/on. Native
source helpers используют public SDK API. Различия version/config проверяются
в adapter-specific compatibility файле; никакого SDK version branching в parser.
Полностью отсутствие HTTPD доказывается dependency graph, link map и symbols
в TCP-only minimal, где приложение также не зависит от HTTPD. Dead stripping
не заменяет корректный список REQUIRES.

## 9. Отложенные возможности и stop gates

Не добавлять в эту реализацию keep-alive, chunked input, TLS, HTTP/2, compression,
асинхронный Response, streaming upload, dynamic routes, replay/coalescing,
универсальную allocator/RTOS HAL или numeric CBOR schemas.

Если требования responsiveness не проходят с последовательными callbacks,
не скрывать это увеличением timeouts/лимитов. Записать измерение и отдельно
изменить execution design: либо предварительный incremental receive c ограниченным
pool, либо явный owned transfer contract. Это разрешаемый архитектурный вопрос,
а не повод молча создать workers или удержать stack Response после callback.

Если budget профиля не помещается на C3, уменьшать число/ёмкость хранилищ явно
и повторять matrix; не обещать max_clients=4 при скрытом лимите admission=2.
Если native SDK не даёт требуемой гарантии, показывать Unsupported/разницу
capabilities вместо имитации. Непроверенный IDF minor остаётся непроверенным.

## 10. Контрольные примеры для реализации

1. Пришло `POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 3\r\n\r\na`,
   затем `bcGET /health HTTP/1.1\r\nHost: test\r\n\r\n`. Dispatcher видит только
   body `abc`, вызывается один endpoint. Следующий request не исполняется; его
   bytes могут быть только отброшены при bounded finish. Header metadata не
   включает body, preparedBody имеет ровно 3 bytes.
2. Отправляются spans `header` и body из 100 B. `writeSome(body)` вернул 17,
   затем WouldBlock, затем 83: caller продолжает с body+17 и size=83.
   Ни повторных первых 17 B, ни full-response copy. Возврат send наступает
   только после 100 accepted bytes или ошибки; это ещё не ACK клиента.
3. Producer передал WS `ABC`, получил Queued и изменил свой buffer на `XYZ`.
   Receiver получает `ABC`. Pool slot освобождается после окончания/отмены
   active send, а не после dequeue. Wire header занимает отдельный short buffer.
4. SSE клиент получил EOF, а `active()` пока равен 1. Это допустимо до local
   completion acknowledgment владельца; bounded wait обязан дождаться 0.
   Не допускать бесконечного расхождения begun/completed после stop success.
5. Client A имел native fd=12 и generation=7. После close клиент B получил
   fd=12, generation=8. Pending job A возвращает Closed и не выполняет send/close
   над B, даже если размер/адрес payload случайно совпали.

При полном accepted connection budget новый accepted socket сразу закрывается
без обещания HTTP 503: для его полноценного response уже нет выделенного session
slot. Это отличается от отказа SSE/WS admission в ещё допустимом HTTP connection,
где bounded 503 до handoff/101 возможен. Не переносить текущий unchecked single
send rejection как доказательство гарантированной доставки error response.

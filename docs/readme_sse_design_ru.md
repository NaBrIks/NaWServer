# SSE: устройство библиотечной реализации

19 октября 2026. Runtime — `src/sse`, публичный API — `sse.h` /
`NaWServer::sse::EventSource`. Код включён в CMake компонента. `tools/sse` содержит
проверки и стенды; `examples/sse` использует библиотеку как приложение.
[Использование](readme_sse_ru.md), [результаты C3/LAN/browser](readme_sse_library_verification_ru.md),
[граница Request/Response](readme_sse_integration_ru.md).

SSE — один длительный HTTP-ответ с текстовыми событиями. Команды клиент отправляет
отдельными HTTP-запросами. История показаний, повторная доставка, JSON и политика
восстановления пропусков принадлежат приложению.

## Формат

Portable encoder C++17 принимает `string_view` с явной длиной и записывает
собственный фиксированный Frame без динамической памяти. Frame ограничен 768
байтами вместе с полями и разделителями. Без метаданных помещается до 760 ASCII
байт data; переводы строк уменьшают этот предел.

| Поле | Контракт |
|---|---|
| data | Корректный UTF-8, включая U+0000; каждая строка получает data:, CR/CRLF нормализуются в LF |
| Пустые data | Создают пустое событие; завершающий LF сохраняется дополнительной пустой строкой data: |
| event | Optional имя; пустое/отсутствующее означает browser message; CR/LF/NUL отклоняются |
| id | Optional строка; отсутствие не меняет id браузера, явная пустая строка сбрасывает его; CR/LF/NUL отклоняются |
| retry | Optional uint32 milliseconds, включая 0 |
| comment | UTF-8, несколько строк, U+0000; heartbeat не создаёт browser event |

Overlong UTF-8, суррогаты, усечённые последовательности и значения выше U+10FFFF
отклоняются. Это текстовый формат, без автоматического binary codec. Encoder
сначала проверяет формат/размер, затем записывает; отказ не меняет Frame. Входные
view заимствуются только на время вызова и не должны ссылаться на выходной Frame.

## Ограниченные очереди и владение

Фиксированные границы: 4 клиента, 3 pending Frame/client, 768 bytes/Frame,
256 bytes/transport step. Runtime Limits могут уменьшать число клиентов и pending;
статический размер объекта остаётся рассчитанным на 4. Payload budget по умолчанию:
2304 bytes/client, 8192 bytes total. Текущее queued событие учитывается до полной
записи. Header/current Frame, metadata, transport step и task stack идут сверх
payload budget.

Producer вызывает publish отдельно для каждого Token. Queued означает принятую
копию, а не доставку/ACK. IndividualOverflow закрывает эту сессию; GlobalFull
отказывает без закрытия клиента; Busy означает краткую конкуренцию за состояние.
Приложение обрабатывает отказы явно. Producer использует try-lock и не ждёт сеть.

Sessions — сериализованная модель без I/O. next выдаёт копию шага с Token generation
и serial; transport работает вне lock, written подтверждает конкретный шаг.
Повторные/stale результаты отклоняются. До ack активной записи слот не освобождается;
повторно использованный fd получает другую generation. При исчерпании uint64
generation слот больше не используется. Сам Token не отменяет уже исполняющийся
send: native request удерживается живым до завершения операции.

## Открытие и транспорт

Endpoint после обычных auth/policy вызывает EventSource.open(Request&,Response&).
Порядок: resolveHeaders → owned preface → reserve → claimExternal → activate.
До claim/activate worker не выдаёт байты. Scoped rollback завершает native copy
без shutdown и возвращает исходному callback возможность обычного ответа.
После External duplicate send/error ставит atomic abort; второй ответ запрещён.
Request, Response, Next и application callback за пределами вызова не сохраняются.

Только GET без body. Last-Event-ID копируется до handoff, ограничен 128 bytes;
присутствующий пустой header отличается от отсутствующего. ResolveHeaders сохраняет
defaults/options/overrides, включая Cache-Control и NoSniff. Полный preface ограничен
768 bytes. MIME фиксирован text/event-stream, Connection: close, граница body — EOF;
Content-Length/Transfer-Encoding/compression/trailer overrides отклоняются.

Backend — native plain HTTPD 5.4, один worker, без TLS/send overrides/внешнего
forced close активных async requests. Он не меняет sess_ctx/free_ctx/transport_ctx.
Worker делает send с MSG_DONTWAIT: EAGAIN/EWOULDBLOCK/EINTR сохраняют offset,
положительная частичная запись продвигает его, zero/fatal закрывают сессию.
Внутренние lwIP locks/планирование остаются; hard real-time/WCET не обещаются.

Heartbeat по умолчанию через 15 s простоя; stall — 2 s без прогресса текущей
передачи; deadline — 5 s на header/событие/heartbeat. Возраст подписки не ограничен.
При закрытии worker прекращает send, делает shutdown, затем ровно один native
async complete. После complete к fd больше не обращается.

## Lifecycle

Конструктор связывает зависимости. Явный start после SDK init создаёт worker и
семафоры. Один экземпляр — один рабочий lifecycle. Порядок владельца: остановить
producers/admission → успешно EventSource.stop(timeout) → server.end → уничтожить
объекты. Если stop вернул false, сохранить объекты/HTTPD и повторить drain.
Не вызывать stop из URI callback и не удалять работающий worker принудительно.

## Источники

- [WHATWG SSE](https://html.spec.whatwg.org/multipage/server-sent-events.html): формат и browser semantics.
- [ESP-IDF 5.4 HTTP Server](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c3/api-reference/protocols/esp_http_server.html): async request и native transport API.
- [IDF v5.4 httpd_txrx.c](https://github.com/espressif/esp-idf/blob/v5.4/components/esp_http_server/src/httpd_txrx.c): allocation/completion и send behavior.
- [IDF v5.4 httpd_sess.c](https://github.com/espressif/esp-idf/blob/v5.4/components/esp_http_server/src/httpd_sess.c): async ownership и session close.
- [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3): граница HTTP body.
- [Локальные лимиты](adr/ADR_20261018-3_nawserver_local-device-limits.md).


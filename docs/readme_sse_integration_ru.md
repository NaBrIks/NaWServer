# SSE и синхронный Request/Response: реализованная граница

19 октября 2026. Runtime — `src/sse`, публичный API — `sse.h` / `sse::EventSource`,
пример — `examples/sse`, тестовый стенд — `tools/sse/integration_device`.
[Библиотечная проверка C3/LAN/browser](readme_sse_library_verification_ru.md),
[предыдущая проверка стенда](readme_sse_integration_verification_ru.md).
Основной HTTP-чат согласовал интеграцию через существующие narrow hooks;
Request/Response не потребовали изменений. Generic deferred HTTP API нет.
SSE входит в source list компонента; runtime запускается явно через start().

## Использование

Terminal endpoint после существующих AuthMiddleware и route policy вызывает
`events.open(request, response)` у `sse::EventSource`. При успехе результат содержит
Token, Last-Event-ID, active=true, result=SendResult::Ok; Response уже External.
Endpoint возвращает SendResult::Ok либо Result::Complete. Открытие синхронное,
долгим потоком затем владеет adapter.

Bridge шаблонизирован только по request/native backend для host fault checks.
В C3 это настоящие Request, Response и внутренний IdfAdapter. Context, Request, Response,
Next и application callback за пределами вызова не сохраняются.

Direct SSE endpoint требует Context или Request/Response. Response-only форма сама
по себе не даёт читать Request. Все три EndpointRef формы проверены на host через
общий executor; host response-only fixture заимствует Request только на время вызова.

## Reserve, claim, activate, rollback

1. Response::resolveHeaders сводит defaults/options/overrides. Указатели немедленно
   копируются encode_headers в собственный bounded Frame: **768 bytes вместе с
   полным HTTP preface**. Header injection, duplicates, unsupported framing/
   compression и overflow отклоняются до reservation.
2. adapter.reserve проверяет GET без body, копирует Last-Event-ID (128 bytes,
   различает absent/empty), резервирует слот, выполняет async begin. Worker ещё
   не обслуживает слот, в том числе не завершает его при concurrent stop.
3. claimExternal({context,abort}) фиксирует владение до первого byte. Обычный
   send/fail после этого вызывает abort и сохраняет External; dispatcher сам
   не закрывает принадлежащий SSE native async request.
4. adapter.activate разрешает worker. Между успешным claim и activate нет
   allocation или application callback. Если stop/abort уже запросил close,
   слот активируется только для Close; bridge возвращает ошибку при External,
   обычный error response больше не разрешён.

Reservation — некопируемый scoped guard. Если claim не состоялся, destructor
выполняет rollback: complete native copy **без shutdown**, освобождение gated слота,
возврат владения исходному callback. Обычный error response остаётся возможен,
если сам Response ещё не имеет sticky ошибки.

Новый bridge устраняет гонку native-only admit, где worker мог начать запись до
учёта Response. Sessions::reserve/activate/rollback протестированы независимо:
100 вызовов next до activate не дают I/O, rollback после shutdown освобождает слот,
activate после shutdown даёт Close, stale/repeated token не применяется.

## Headers

MIME фиксирован: text/event-stream. Framing: Connection: close, тело до EOF.
Дополнительные Content-Length/Transfer-Encoding/Connection/Content-Type запрещены.
Content-Encoding и Trailer также отклоняются: adapter не сжимает и не выдаёт trailers.
Остальные разрешённые merged headers копируются. Cache-Control по умолчанию no-cache;
явная policy/override сохраняется. JSON ContentType из общих options не меняет SSE MIME.
Wire проверен с локальной строкой override после возврата endpoint.

Encoder, UTF-8/NUL/id/retry и bounded publish описаны в
[основном guide](readme_sse_design_ru.md). Queued не означает peer/application ACK.

## Abort и lifetime

ExternalControl указывает на стабильный слот adapter. Abort делает atomic store
флага, не ждёт mutex/сеть. Worker потребляет флаг и закрывает соответствующий Token.
Adapter привязан к **одному native HTTPD instance**; чужой handle отклоняется.
Response/control живут только внутри исходного HTTPD callback: этот HTTPD не сможет
начать следующий callback и переиспользовать reservation до уничтожения Response.
Нельзя сохранять ExternalControl или вызывать его из последующих запросов.

После handoff duplicate send и error middleware после next вызывают abort.
Уже отправленные bytes не отзываются: close может быть до header или после части
события. Второй HTTP 500/обычный body не выдаётся. Generation/serial защищают
выданные транспортные шаги; atomic abort их не заменяет.

## Stop и пределы

Владелец прекращает producers/admission, ждёт успешный adapter.stop, затем server.end,
затем уничтожает handlers/adapter. stop(false) сохраняет объекты/HTTPD живыми.
Незавершённая reservation должна дойти до activate/rollback; task насильно не удаляется.
HTTP control в примере только ставит флаг, stop выполняет application owner task.

Transport: plain native HTTPD 5.4 без TLS/send overrides/external forced close
active requests. Одна task, шаг 256 bytes с MSG_DONTWAIT, shutdown до ровно одного
async complete, затем никаких обращений к fd. История/replay/coalescing и worker pool
не добавлены. SDK heap OOM fault injection и TLS не проверены.

После библиотечного прогона восстанавливается minimal и выполняются оба HTTP/WS
и HTTP API smoke. Фактическая прошивка и результаты — в библиотечном отчёте.


# SSE в библиотеке: EventSource

Публичный `sse.h` не зависит от SDK. Общие encoder, Sessions, bridge и EventSource
находятся в `src/sse` / `include/NaWServer/detail/sse`; backend выбирается явно.
Текущий сетевой адаптер — общий TCP для ESP-IDF, Windows, Linux и RP; также есть Memory и диагностический Windows backend. [Контракт адаптеров](readme_sse_adapters_ru.md).
В `tools/sse` только тесты/стенд, без второй копии реализации.
[Проверка библиотечной версии](readme_sse_library_verification_ru.md).

## Использование

```cpp
#include <NaWServer/sse.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/backends/tcp.h>
#include <NaWServer/request.h>
using namespace NaWServer;
extern TcpHttpBackend http_backend; // the same backend used by HttpServer
static sse::TcpAdapter backend(http_backend);
static sse::EventSource events(backend); // no runtime in constructors

// Call events.start() after SDK init. Install behind Route auth/policy.
SendResult subscribe(Request& request, Response& response) {
    auto opened = events.open(request, response);
    if (opened.result == SendResult::Ok) {
        // Save opened.token; copy opened.last_id for application logic if needed.
        return SendResult::Ok;
    }
    if (response.state() == ResponseState::External) return opened.result;
    return response.text(opened.admission == sse::Admit::Full ? 503 : 400,
                         "SSE admission rejected");
}

void publish(sse::Token token) {
    sse::Frame frame;
    if (sse::encode({"line1\nline2", "reading", "boot:42", 1000}, frame)
            != sse::EncodeResult::Ok) return;
    auto result = events.publish(token, frame);
    // Application handles Queued, Busy, GlobalFull, IndividualOverflow, Closed.
    (void)result; // Queued is NOT delivery/application ACK.
}
```

Готовый компактный пример — `examples/sse`: GET `/events` и POST `/publish`
с существующим Basic provider/pipeline; GET `/health` публичный. Публичный fixture
`test:test-only` не является production credential. Wi-Fi/NVS принадлежат приложению.

```powershell
curl.exe -N -u test:test-only http://DEVICE/events
curl.exe -u test:test-only -X POST --data-binary "hello" http://DEVICE/publish
```

Порядок владельца: SDK init → events.start → server.begin → работа; затем остановить
publishers/admission → успешно events.stop(timeout_ms) → server.end → уничтожить
handlers/events. stop(false) сохраняет владение: повторить drain, не удалять task
и не останавливать HttpServer раньше времени. Не вызывать stop в URI callback.
Само подключение библиотеки не создаёт задач/соединений. Minimal и Windows mini UI
явно запускают SSE для диагностики HTTP/WS/SSE.
Экземпляр имеет один рабочий lifecycle: после успешного stop повторный start
отклоняется; для нового запуска нужен новый экземпляр после освобождения старого.

## Контракт и ограничения

- Один экземпляр: максимум 4 клиента; Limits могут уменьшить число до 1–3. Default:
  3 pending/client, 768 bytes/encoded Frame, queued bytes 2304/client, 8192 total.
  Объект всё равно рассчитан на 4 слота; task stack/SDK allocations сверх payload budget.
- UTF-8 и явные длины, multiline/empty/trailing LF, event/optional string id/retry,
  comment heartbeat. NUL разрешён в data/comment, запрещён в event/id; CR/LF в
  event/id отклоняются. Absent id и явно пустой id различаются. Это не binary codec.
- Last-Event-ID копируется до ответа, предел 128 bytes, absent/empty различаются.
  Нет history/replay/coalescing. Encoder не in-place: вход не ссылается на выходной
  Frame; failure не меняет Frame. Готовый HTTP preface также ограничен 768 bytes.
- GET без body, открытие после обычного auth/policy. Успешный publish копирует
  Frame; producer использует try-lock и не ждёт сеть. Individual overflow/stall
  закрывает эту сессию, GlobalFull/Busy — явные отказы без обвинения клиента.
- Idle heartbeat (15 s), no-progress stall (2 s), deadline события (5 s) различаются;
  общий возраст подписки не ограничен. Один worker отправляет по 256 bytes с
  MSG_DONTWAIT через платформенный transport. Worker уступает CPU минимум на один RTOS tick, в том числе
  при CONFIG_FREERTOS_HZ=100. Внутренние lwIP locks/планирование остаются, WCET не обещается.

Request/Response остаются синхронными. Внутри open: resolveHeaders → own header →
reserve → claimExternal → activate. До claim нет SSE bytes. Duplicate send/error
после next вызывает abort вместо второго ответа. Request/Response/Next/callback
не сохраняются; reserve/activate скрыты из обычного EventSource API.
[Детали bridge](readme_sse_integration_ru.md).

Общий TCP backend использует платформенные sockets и ограниченные WS/SSE runtime.
`Route` принимает отдельный `HeaderPipeline` для auth/quota до чтения тела;
после успешного SSE открытия lease квоты передаётся подписке до завершения.
`Drop` отменяет незавершённую передачу через владельца I/O, без второго ответа.
Резерв/лимиты HTTP, WS, SSE и общей TX-очереди согласуются приложением.
Четыре логических клиента и четыре TCP sockets — разные ограничения.
TLS и произвольное отложенное завершение обычного HTTP не предоставляются.
При включённой общей TX-очереди кадры SSE используют её блоки; отдельная legacy
payload-очередь не дублирует их: [подробнее](readme_sse_shared_transmission_ru.md).

## Источники

Спецификация: [WHATWG SSE](https://html.spec.whatwg.org/multipage/server-sent-events.html).

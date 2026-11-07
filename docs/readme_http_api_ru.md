# HTTP API: маршруты, Request и Response

Актуальный контракт общей реализации, 3 ноября 2026. Нативный ESP-IDF HTTPD удалён:
Windows, Linux, ESP-IDF и RP используют общий HTTP/WS/SSE двигатель с платформенным
TCP-адаптером. Старые отчёты `*_verification_ru.md` описывают датированные проверки,
а не подтверждают новую прошивку.

## Владение и запуск

`HttpServer` из `<NaWServer/http_server.h>` заимствует `HttpBackend`, статическую
таблицу обработчиков и их зависимости. Конструкторы не запускают задачи или сеть.
Приложение сначала инициализирует платформу и сеть, затем вызывает `begin()`;
перед уничтожением зависимостей вызывает `end()` и проверяет успех. Остановка из
callback не поддерживается. При `CleanupRequired` зависимости сохраняются до
успешного повторного `end()`.

```cpp
#include <NaWServer/backends/memory.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>

using namespace NaWServer;
SendResult health(Response &response)
{
    return response.text(200, R"({"ok":true})");
}
MemoryHttpBackend backend;
Route health_route("/health", HttpMethod::Get, health);
HandlerBase *const routes[]{&health_route};
HttpServer server(backend, routes);
```

`MemoryHttpBackend` — последовательный тестовый backend с готовыми metadata.
Реальная сеть использует `TcpHttpBackend` и `tcp::WindowsTransport`,
`tcp::LinuxTransport`, `tcp::IdfTransport` либо `tcp::LwipTransport` из
соответствующего заголовка `backends/*_tcp.h`. Платформенные предпосылки и конфигурации
описаны в [руководстве переносимой сборки](readme_portable_build_ru.md).

## Выбор маршрута и ранняя обработка

Порядок: ограниченный разбор заголовков → декларативный выбор маршрута → ранняя
цепочка → получение тела → прикладная цепочка и endpoint. Для WS ранняя цепочка
выполняется перед `101 Switching Protocols`, для SSE — перед открытием потока.
Проверки клиента не находятся в TCP accept: там остаются физические лимиты,
таймауты и владение сокетом.

`canHandle` удалён. `HandlerBase::routes()` возвращает заимствованную таблицу
`RouteDescriptor {path, method, match}`; `match` — `RouteMatch::Exact` или `Prefix`.
`Route` предоставляет один точный маршрут. Точный путь имеет преимущество перед
префиксом; среди префиксов выбирается самый длинный. Query не участвует в выборе.
При равной специфичности первый подходящий метод в таблице выигрывает.
Если самый специфичный путь существует, но метод не поддерживается, сервер отвечает
405 с `Allow`, не переходит к более общему префиксу. Неизвестный путь даёт 404;
`onNotFound` позволяет задать собственный ограниченный обработчик.

Пример декларации группы обработчиков:

```cpp
RouteDescriptors routes() const override
{
    static const RouteDescriptor entries[]{
        {"/api", HttpMethod::Get, RouteMatch::Exact},
        {"/assets/", HttpMethod::Get, RouteMatch::Prefix}
    };
    return {entries, 2};
}
```

Строки и таблица живут дольше сервера. Префикс — буквальный префикс строки:
`/api` также совпадёт с `/apix`; для группы каталогов задавайте `/api/`.
Действия, зависящие от JSON/CBOR, выбирает приложение внутри endpoint либо разделяет
по маршрутам; библиотека не анализирует тело для выбора обработчика.

Полная форма `Route`:

```cpp
Route(path, method, endpoint, application_pipeline, access_policy,
      response_options, header_pipeline, body_options);
```

Последние аргументы необязательны. Общую раннюю цепочку передают последним аргументом
конструктора `HttpServer` или через `setHeaderPipeline()` до `begin()`.
`headerPipeline()` обработчика и глобальная цепочка используют один исполнитель; глобальная цепочка оборачивает цепочку маршрута,
проверка доступа завершается после обеих. Подробности порядка и auth —
[в руководстве middleware](readme_middleware_ru.md).

## Два режима входящего тела

| Режим `BodyOptions::mode` | Контракт |
|---|---|
| `Buffered` (по умолчанию) | Endpoint получает полное ограниченное `request.body()` |
| `None` | Непустое тело отклоняется до чтения |
| `Stream` | Backend подаёт ограниченные блоки `BodyReceiver`, не собирая всё тело |

`max_bytes=0` выбирает `HttpLimits::max_body_size` (по умолчанию 4096).
Явный `max_bytes` задаёт предел маршрута. Превышение даёт 413 до чтения тела.
Потоковый маршрут может разрешить, например, 1 МиБ при рабочем блоке 1024 байта:
это независимые размеры. `block_bytes` по умолчанию равен
`NAWSERVER_HTTP_BODY_BLOCK_BYTES` (1024).

```cpp
BodyOptions upload_body;
upload_body.mode = BodyMode::Stream;
upload_body.max_bytes = 1024 * 1024;
upload_body.block_bytes = 1024;
upload_body.context = &bounded_upload_pool;
upload_body.begin = begin_upload; // BodyReceiver(void *, HttpExchange &)
Route upload("/upload", HttpMethod::Put, finish_upload, {},
             AccessPolicy::authenticated(), {}, private_chain, upload_body);
```

`begin` вызывается после ранних проверок и возвращает move-only `BodyReceiver`.
Приложение выдаёт состояние из ограниченного пула; библиотека не монтирует FS и не
решает, записывать ли блок в файл, SD, OTA-раздел или иной приёмник.

- `consume(context, bytes)` возвращает `Accepted` для всего блока, `Pause` для
  нуля принятых байт либо `Error`. При `Pause` тот же блок будет предложен повторно.
- Переданный `ByteView` заимствован только на время callback. Сохранять его нельзя.
- `finish` подтверждает успешное завершение тела; `cancel` прерывает незавершённый
  приём; `release` освобождает прикладное состояние. Владение возвращается ровно один раз.
- Endpoint вызывается после успешного завершения; `body()` у него пустой,
  `contentLength()` сохраняет исходную длину, `bodyMode()` сообщает `Stream`.
- Callback приёмника должен выполнять короткий ограниченный шаг. Ожидание сети или
  готовности приёмника отдаёт управление другим соединениям; долгий прикладной
  callback всё равно задерживает сериализованные callbacks.

Потоковый вход реализован управляемым TCP backend. Backend без этой возможности
возвращает 501; скрытого перехода к буферизации большого тела нет.
Chunked upload и `Expect: 100-continue` не поддерживаются; используется Content-Length.

## Request

`Request` и `Response` принадлежат одной операции. Их нельзя сохранять после callback.
`Request` расширяет ранний `HeaderRequest`:

- `method()`, `path()`, `rawQuery()`, `contentLength()`; path/query — borrowed views.
- `readHeader(name, destination, capacity)` возвращает статус и длину без NUL;
  capacity включает NUL. Неоднозначный повтор запрошенного заголовка отклоняется
  общим TCP parser. `header(name)` возвращает удобную копию `optional<string>`.
- `body()`/`copyBodyTo()` работают с buffered телом; бинарные данные сохраняются.
- `query()`, `form()`, `parameter()` возвращают `optional<string_view>`; последняя
  форма предпочитает form значению query. Пустое значение отличается от отсутствия.
- `peerAddress()` возвращает доступный IPv4 в host byte order, а
  `transportExtension()` — явно типизированную адаптером границу расширений.

Ранний `HeaderRequest` не предоставляет body/form API. Повторный этап использует уже
выбранный маршрут и проверенную identity; не выбирает обработчик заново по телу.

## Ответы и память

```cpp
return response.text(200, "ready");
return response.send(200, "application/cbor", request.body());
return response.empty(204);
return response.redirect("/health");
```

Это альтернативы одного итогового ответа. Поддерживаются endpoints
`SendResult(Request &, Response &)`, `SendResult(Response &)` и `Result(Context &)`.
`send`/`text`/`bytes` потребляют заимствованный буфер до возврата; очередь при
необходимости владеет собственной копией. `Ok` означает принятие транспортом,
не TCP ACK и не обработку браузером.

| Источник ответа | API и время жизни |
|---|---|
| Малое готовое тело | `send`/`text`/`bytes`; вход нужен до возврата |
| Неизменяемая память | `sendImmutable`; память живёт до остановки сервера |
| Выделенный блок менеджера | `sendOwned`; владение передаётся до завершения/отмены |
| Заимствованный reader | `sendSource`; синхронный, источник нужен до возврата |
| Файл/источник с владением | `sendOwnedSource(OwnedResponseSource)`; короткие шаги между другими запросами |
| Синхронный производитель | `beginStream`/`write`/`finish`; callback остаётся занятым до возврата |

`OwnedResponseSource` содержит известную длину, callback чтения и `release`;
последний закрывает источник при успехе, ошибке или Drop. Это состояние передачи,
а не отложенный произвольный HTTP endpoint. Для большого файла и отзывчивого API
используйте owned source; одна TX-задача не превращает долгий `sendSource` в
неблокирующий прикладной callback.

`Response::acquire()` позволяет заполнить выданный блок непосредственно. До commit
он не отправляется; lease нужно завершить или отменить до `finish`/уничтожения Response.
Очередь и ограничители памяти описаны отдельно в руководствах TX и memory manager.

`TextBuffer<N>` — независимый ограниченный builder с `print`/`printf`; переполнение
запоминается, неполный результат не отправляется. Это не сетевой поток.

## Заголовки, ошибки и завершение

`DefaultHeaders` — неизменяемая borrowed таблица. `ResponseOptions` задаёт MIME,
cache policy и независимые флаги; профиль маршрута заменяет серверный целиком.
Приоритет: defaults → options → `response.header`. Имена сравниваются без регистра.
Индивидуальные имя/значение копируются в ограниченную arena; скрытого heap fallback нет.
Максимум — 16 дополнительных заголовков и 768 байт индивидуальных строк.
Content-Length, Transfer-Encoding и Connection принадлежат backend.

`SendResult` сообщает ошибки настройки/лимитов/транспорта; после ошибки отправки
повторный ответ запрещён. Callback без ответа даёт 500. Ошибка после начала ответа
закрывает соединение. `Result::Drop` или `response.drop()` — терминальное состояние:
никакого fallback/финального flush, закрытие и освобождение выполняет I/O owner.
Внешнее middleware не может отменить Drop возвратом `Complete`.

Обычный `Request` нельзя удерживать до завершения произвольной фоновой работы.
Такая работа возвращает job id и использует последующий polling/WS/SSE;
прикладной пул работ тоже ограничивается. Keep-alive сохраняется и не означает
сохранение рабочих буферов между запросами.

## Проверки

`tests/header_routing_test.cpp` проверяет ранний auth, выбор маршрута, Drop,
границы отказа и время жизни квоты; `pipeline_test.cpp` — единый исполнитель,
порядок/однократность next и отсутствие выделений в цепочке. TCP-тесты отдельно
проверяют потоковый вход, владение выходом и прогресс других соединений.
Сборка и host-проверка не заменяют аппаратную проверку конкретной прошивки.

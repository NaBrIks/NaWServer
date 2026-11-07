# Общие HTTP/WS/SSE через TCP

Текущий этап: P6 в работе, 25 октября 2026. Общие HTTP/1.0/1.1, WS wire engine и SSE
работают на Windows/Linux; добавлены конкретный IDF/lwIP transport, private FreeRTOS
worker и выбор TCP/HTTPD при сборке. Route/middleware/handlers, Request/Response
и canonical SSE encoder/Sessions/bridge сохраняются. Native HTTPD использует свой
SDK wire/runtime. P6 ещё не закрыт; функциональные наблюдения C3, SDK-сборки,
нагрузочная проверка и длительная приёмка учитываются отдельно.
Текущий приоритет владельца — рабочий MVP на C3; Windows/Linux используются для
быстрых направленных проверок. Матрица из 16 SDK-профилей ниже относится к checkpoint
до safer-close; текущий код пересобран только в двух полных TCP-профилях SDK5/6.
Свежие проверки и расходы: [поэтапный отчёт](readme_tcp_engine_verification_ru.md).

## Композиция на host

### Остановка и повторный запуск, проверка 02.11.2026

Порядок полного выключения собственного веб-стека:

1. Приложение запрещает новые публикации HTTP/WS/SSE и дожидается завершения
   уже начатых producer-вызовов. Не держать producer lock во время join/stop.
2. Дождаться `events.stop(timeout_ms) == true`.
3. Дождаться `server.end() == HttpError::Ok`.
4. Выполнить `transport.stop()` и проверить успешный результат.

При таймауте или `CleanupRequired` зависимости сохраняются до успешной очистки;
restart и уничтожение живых owners недопустимы. Остановку выполняет прикладной owner,
а не HTTP/WS callback. Обратный порядок запуска: `transport.start()`, `events.start()`,
`server.begin()`, затем разрешение producers. Повторный `transport.start()` на уже
запущенном transport возвращает `InvalidState`: приложение отслеживает lifecycle.
Не переиспользовать SSE tokens старого поколения после restart.

Успешный SSE stop дожидается завершения worker и вызывает `releaseStorage()`:
освобождаются динамические frame sets, первоначальный резерв и таблица memory pool.
Объект адаптера со статическими метаданными остаётся жив. Успешный HTTP stop закрывает
WS и освобождает runtime/буферы. Внешние удержанные WS-объекты могут продлевать жизнь
своего allocation context; приложение управляет этими ссылками отдельно.
`IdfTransport::stop()` не останавливает системный lwIP или сетевые интерфейсы.

В `tcp_transport_integration_test` проверены одновременно REST, WS и SSE,
3 полных цикла в четырёх профилях: legacy/managed TCP × начальный SSE-резерв 0/1.
Пользовательские counting allocators проверяют реальное освобождение блоков,
а не только обнуление счётчика backend. Сводка host requested payload/array bytes:

| Профиль | TCP Windows / Linux, Б | SSE, Б | Остаток после успешной остановки |
|---|---:|---:|---:|
| Legacy TCP, один WS и один SSE | 34 822 / 34 822 | 3 200 | 0 блоков / 0 Б |
| Managed TCP, один WS и один SSE | 38 332 / 38 012 | 3 200 | 0 блоков / 0 Б |

Числа одинаковы в трёх циклах и при начальном SSE-резерве 0/1 после подключения
одного клиента. Они не включают control objects, стеки worker, STL и системные
socket allocations; это не оценка освобождённой ESP RAM/PSRAM. На устройстве эти
области замеряют отдельно средствами SDK. Новые tokens, обмен после restart,
нулевой остаток owned sockets, HTTP/SSE buffer stats и allocator blocks проходят.
Направленный прогон integration/lifecycle/SSE-worker: 3/3 PASS на Windows и Linux,
менее двух минут; SSE-worker дополнительно проверяет 100 собственных start/stop.

```cpp
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#if defined(_WIN32)
#include <NaWServer/backends/windows_tcp.h>
#else
#include <NaWServer/backends/linux_tcp.h>
#endif
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <NaWServer/sse.h>
#include <NaWServer/websocket.h>

using namespace NaWServer;

#if defined(_WIN32)
using HostTransport = tcp::WindowsTransport;
#else
using HostTransport = tcp::LinuxTransport;
#endif

SendResult health(Response &response)
{
	return response.text(200, "ready");
}

struct OpenEvents
{
	sse::EventSource &events;

	SendResult operator()(Request &request, Response &response)
	{
		const auto opened = events.open(request, response);
		// Transfer opened.token to synchronized publisher state when opened.active.
		if(opened.result == SendResult::Ok || response.state() == ResponseState::External)
			return opened.result;

		return response.text(opened.admission == sse::Admit::Full ? 503 : 400,
		           "SSE admission rejected");
	}

};

struct Application
{
	HostTransport transport;
	TcpHttpBackend backend{transport};
	sse::TcpAdapter sse_adapter{backend};
	sse::EventSource events{sse_adapter};
	OpenEvents open_events{events};
	Route health_route{"/health", HttpMethod::Get, health};
	Route events_route{"/events", HttpMethod::Get, EndpointRef(open_events)};
	WebSocketEndpoint websocket{"/ws"};
	HandlerBase *const routes[3]{&health_route, &events_route, &websocket};
	HttpServer server{backend, routes};
};

Application application; // Constructors bind owners; runtime starts explicitly.
```

В примере `/events` публичный для локального стенда. Для закрытого потока передать
в Route существующие auth chain/access policy: они выполняются до `events.open()`.
Это не защищает отдельный WS upgrade; authenticated WS по-прежнему не поддержан.
WebSocketEndpoint без `onMessage` только допускает соединения; callbacks и проверка
результатов `send`/`send_binary` принадлежат приложению.

Конструкторы не запускают сеть/потоки. Порядок запуска владельцем:

1. `application.transport.start().status == tcp::Status::Ok`.
2. `application.events.start() == true`.
3. `application.server.begin() == HttpError::Ok`.

EventSource можно запустить до HTTP begin. Он заимствует TcpAdapter, а TcpAdapter —
конкретный TcpHttpBackend; поток от другого backend будет отклонён до retain.
Transport заимствуется HTTP backend. Handlers, middleware, chains и callback data
живут до успешного `end()`. При неудачном запуске также проверить и завершить
уже запущенные ресурсы, сохраняя владельцев при неудачном cleanup.

Остановка: прекратить publishers/admission → успешно `events.stop(timeout_ms)` →
`server.end() == HttpError::Ok` → `transport.stop().status == tcp::Status::Ok`.
Если drain вернул false, сохранить EventSource/adapter/backend/transport и повторить
drain; HTTP end при активной SSE registration возвращает InvalidState. При timeout
HTTP end возвращает CleanupRequired. Нельзя уничтожать владельцев или останавливать
transport после неуспешного end. Stop/end выполняются вне callbacks. Никакого
автоматического fallback на прежний Windows или IDF backend нет.

`TcpOptions::bind_address` копируется в16-byte storage при конструировании.
Поддержан numeric IPv4; default127.0.0.1, port8080. Port0 выбирает ephemeral port,
доступный через `backend.port()` после begin. Остальные defaults: backlog7,
no-progress1000ms, wait ceiling20ms, stop timeout5000ms, max_websocket_clients4,
websocket_send_timeout_ms1000, websocket_tx_blocks4/websocket_tx_block_size4096.
Это один общий TX pool16384 B на backend для всех WS endpoints, включая active
frames. Endpoint с большим max_message_size требует явно большего block_size;
несовпадение отклоняется до listener. HTTP-only не выделяет backing storage pool.
HTTP limits задаются
прежним HttpLimits, default head8192/header count32/target1024/body4096/connections7,
exchange deadline5000ms. Это отдельные OS и storage budgets; backlog не равен
точному количеству активных сокетов.

## Композиция ESP-IDF и выбор сборки

Общий `HttpServer` заимствует выбранный backend. Для TCP это `TcpHttpBackend` и
конкретный `tcp::IdfTransport`; для HTTPD — `EspIdfHttpBackend`. `Server` остаётся
native HTTPD facade и используется в HTTPD-сборке: переключатель Kconfig не
превращает его в TCP server. Общего Wi-Fi/RTOS/storage HAL нет.

Минимальная HTTP-композиция; инициализация SDK и вызовы start/begin выполняются
позже из application owner:

```cpp
#include "sdkconfig.h"
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#if CONFIG_NAWSERVER_BACKEND_TCP
#include <NaWServer/backends/esp_idf_tcp.h>
#include <NaWServer/backends/tcp.h>
#else
#include <NaWServer/backends/esp_idf.h>
#endif

using namespace NaWServer;

#if CONFIG_NAWSERVER_BACKEND_TCP
static tcp::IdfTransport transport;
static TcpOptions makeTcpOptions()
{
	TcpOptions options;
	options.bind_address = "0.0.0.0";
	options.port = 80;
	return options;
}

static TcpHttpBackend backend{transport, makeTcpOptions()};
#else
static EspIdfHttpBackend backend{80};
#endif

static SendResult health(Response &response)
{
	return response.text(200, "ready");
}

static Route health_route{"/health", HttpMethod::Get, health};
static HandlerBase *const routes[]{&health_route};
static HttpServer server{backend, routes};
```

Для SSE добавить явный adapter и EventSource, затем обычный Route с `events.open`,
как в host-примере. В TCP-сборке это `sse::TcpAdapter{backend}` из
`<NaWServer/backends/tcp_sse.h>`, в native — `sse::IdfAdapter` из
`<NaWServer/backends/esp_idf_sse.h>`. `sse::EventSource` заимствует выбранный adapter.
WS подключается прежним `WebSocketEndpoint`; callbacks и ограничения задаёт
приложение до begin. Полная композиция с общими demo routes находится в
[minimal sample](../examples/minimal/main/main.cpp); он требует включённых WS и SSE.

Конструкторы не создают сокеты или задачи. Application owner сначала выполняет
SDK prerequisites, включая `esp_netif_init()`, и отдельно настраивает сеть.
После этого: TCP `transport.start()` → при наличии SSE `events.start()` →
`server.begin()`. Проверять результат каждого шага; HTTPD не требует TCP transport.
Инициализация Wi-Fi, credentials, event loop, NVS и монтирование storage остаются
за приложением. Для доступа из LAN TCP bind нужно явно сменить с loopback на
нужный numeric IPv4, например `0.0.0.0`.

Остановка владельцем: прекратить publishers и application poll/admission →
успешно drain `events.stop(timeout_ms)` → успешно `server.end()` → для TCP успешно
`transport.stop()`. При false/CleanupRequired сохранить зависимости и повторять
только разрешённый cleanup. Не выполнять end из HTTP/WS callback. Native и TCP
SSE lifecycle не взаимозаменяемы; TCP restart opt-in описан ниже.

В IDF menuconfig появился раздел `NaWServer`:

| Параметр | Назначение |
|---|---|
| `CONFIG_NAWSERVER_BACKEND_HTTPD` | Native HTTPD; default choice |
| `CONFIG_NAWSERVER_BACKEND_TCP` | Общий TCP backend; выбрать ровно один backend |
| `CONFIG_NAWSERVER_WEBSOCKET` | WS выбранного backend; default y при доступной capability |
| `CONFIG_NAWSERVER_SSE` | SSE encoder/queue и adapter выбранного backend; default y |

Native WS также требует `CONFIG_HTTPD_WS_SUPPORT=y`: NaWServer не включает этот SDK
флаг автоматически. **Для SDK с API post-handshake callback, включая IDF 5.5.6
и 6.x, дополнительно нужен `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`.**
Эти SDK после HTTP 101 не вызывают обычный URI handler; открытие сессии
привязывается через post-handshake callback. Без флага сборка явно отклоняется:
CMake проверяет API выбранного SDK, а не только major version. В ранее
проверенном SDK 5.4.0 этот API отсутствует и параметр не используется.
TCP WS не зависит от обоих HTTPD-флагов. При отключённом SSE не компилируются его
encoder/queue/adapters; WS-only TCP не подтягивает SSE encoder.

Пример отдельного `sdkconfig.tcp-http.defaults` для HTTP-only:

```text
CONFIG_NAWSERVER_BACKEND_TCP=y
# CONFIG_NAWSERVER_BACKEND_HTTPD is not set
# CONFIG_NAWSERVER_WEBSOCKET is not set
# CONFIG_NAWSERVER_SSE is not set
```

В активированном окружении нужного SDK, из application project:

```text
idf.py -B build/tcp-http -DIDF_TARGET=esp32c3 -DSDKCONFIG=sdkconfig.tcp-http "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.tcp-http.defaults" build
```

SDKCONFIG и build directory должны быть отдельными для каждого профиля. Существующий
SDKCONFIG с выбором backend имеет приоритет над defaults; отсутствие новой настройки
позволяет использовать defaults и их target-specific sidecars. Противоречивый выбор
отклоняется. IDF раскрывает component requirements до Kconfig, поэтому ранний выбор
зависимостей дополнительно сверяется с итоговым config; при расхождении требуется
reconfigure, сборка с неверным набором зависимостей не принимается. Для параллельных
SDK5/SDK6 проверенная процедура использует также отдельные application project
directories, чтобы component manager не перезаписывал общий `dependencies.lock`.

Для standalone минимального проекта граф ограничивался `set(COMPONENTS main)` до
`include($ENV{IDF_PATH}/tools/cmake/project.cmake)`. Его main component требует
`NaWServer` и `esp_netif`. TCP branch самого компонента требует только
`freertos/lwip/esp_timer`, без `esp_http_server` и `esp-tls`. Отсутствие HTTPD/TLS
проверено в фактических graphs/ELF; другие зависимости настоящего application
project могут самостоятельно добавить эти SDK components. Native `Server` либо
прямое использование HTTPD не относится к TCP-only composition.

На IDF `IdfTransport` повторно задаёт конечный `SO_SNDTIMEO=50ms` перед shutdown/close.
Обычный cleanup больше не вызывает отдельный shutdown: HTTP/WS выполняют bounded
discard до одного close владельцем, SSE — registry close. Сам Transport.shutdownWrite
сохраняет настоящую семантику half-close для явных вызовов и не заменён на no-op.
Это ограничивает lwIP FIN-memory-pressure wait, но не задаёт hard50ms: SDK TCP poll
около500ms, mailbox/scheduler и пользовательский callback добавляют своё время.
Guard failure не вызывает native close; неизвестная ошибка сохраняет владение.
Нет linger0, private TCP abort или бесконечного retry.

## Подмножество протокола и lifetime

Обычный HTTP: один request на connection, Content-Length, Connection:close.
Успешные WS/SSE переходят к отдельному владельцу соединения. HTTP и WS callbacks
последовательны; один HTTP workspace переиспользуется после окончания callback
и синхронного send. В исходном этапе keep-alive отсутствовал; текущий опциональный bounded keep-alive описан в readme_20261027_memory_manager_keepalive_ru.md. Pipelining, chunked upload, Expect, TLS и streaming
upload не реализованы. Подробная policy отказов — в
[контрактах](plans/PLAN_20261024_nawserver_tcp-engine-contracts.md).

Request видит полностью прочитанное bounded body. TCP читает в один переиспользуемый
body workspace и передаёт borrowed view: coalesced prefix копируется один раз,
остальные bytes читаются сразу в конечные offsets. `HttpExchange::preparedBody()`
по умолчанию возвращает nullopt; прежние backends сохраняют owned vector/readBody.
Present empty view означает подготовленное пустое тело, malformed size/null pointer
отклоняется без fallback. Raw body/query/form semantics сохраняются. Синхронные
response metadata/payload заимствуются до возврата send. Retaining Request,
Response, middleware Next или их views после callback запрещён.

Read EOF входящей половины не запрещает ответ на полностью полученный request.
Partial writes продолжаются с правильного offset; ошибка после начала response
не запускает второй HTTP response. Bounded transport wait не прерывает
произвольный блокирующий код endpoint. Пока callback занят, остальные HTTP
и WS callbacks ждут; worker pool или скрытый recursive dispatch не добавлены.

Stop вызывается владельцем вне callback. Timeout возвращает CleanupRequired,
сохраняя ресурсы для повторного end после завершения callback. Успешный end
гарантирует отсутствие поздних callbacks. Не уничтожать transport/handlers/backend
после неуспешной остановки. При ambiguous Fatal close backend удерживает cleanup;
автоматический retry с риском закрыть переиспользованный descriptor запрещён.

## WebSocket и его расходы

`WebSocketEndpoint` остаётся общим. TCP backend проверяет HTTP handshake, готовит
session/client/container node до101, полностью отправляет101 и только затем вызывает
исходный HandlerBase для opening/commit. Нельзя подменять этот порядок прямым вызовом
endpoint. При ошибке до101 reservation откатывается без публикации активного клиента.

Общий decoder проверяет masking/RSV/opcode/длины, fragmented messages, UTF-8,
control/close. Сообщение собирается и снимает mask в одном RX buffer. WS хранит
одну копию фактического raw HTTP head и индекс фактических headers с момента
допуска: исходный HTTP workspace сразу может использоваться следующим запросом.
Request/Response/client/data pointers и views нельзя удерживать после callback.

Defaults: максимум4 WS на TCP backend и4 клиента на endpoint,4096 байт на сообщение,
4 queued-or-inflight сообщения на клиента. `WebSocketLimits` и `TcpOptions` задают
разные пределы; общий `HttpLimits::max_connections` также учитывает HTTP/WS/SSE.
`sendCopy` копирует payload один раз, затем ownership перемещается в active frame;
header отправляется отдельным span, без второй contiguous-копии всего кадра.
Queued означает приём, не доставку. TCP producer возвращает Busy при try-lock
contention, GlobalFull при занятом общем pool, QueueFull при индивидуальном лимите.
Busy/GlobalFull сохраняют соединение; QueueFull запрещает новые sends и запрашивает
close1013 через I/O owner. Уже частично переданный frame завершается либо закрывается
по deadline; close не вставляется в его середину. Native/memory/прежний Windows
имеют отдельно проверяемые queue semantics и не получают pool автоматически.

P4 резервирует16 KiB TX payload на backend при старте вместо промежуточного P3
потолка64 KiB на4 WS. Даже пустое сообщение занимает блок; queued+active учитываются
вместе. Block освобождается только после send completion/abort, не после dequeue.
При полном pool скрытого выделения на heap нет. Абсолютный срок активного output
начинается до ожидания writable; progress не продлевает его. В compiler-only C3
профиле WS object/RX/pending занимают5152 B usable; array cookie и client/controlblock/
container node доводят известные allocation requests до5208 B плюс actual head
и16×actual header count. Pool payload/metadata, allocator и worker учитываются
отдельно. Это ABI-оценка, не SDK runtime heap measurement; полный ledger — в отчёте.

Историческая P5-ревизия добавила один shutdownWrite и последующий bounded TCP drain,
чтобы убрать Linux RST при непрочитанных mask bytes после раннего WS reject.
В текущем safer-close HTTP/WS сначала выполняют incremental discard до 4096 B/50 ms,
затем владелец вызывает один close, без промежуточного SHUT_WR. Так cleanup избегает
раздельного half-close/close lifetime в lwIP. Это не объявление доказанного исправления
вендорного дефекта для всех SDK: причины и отдельные runtime-свидетельства — в отчёте.
HTTP сохраняет scratch 256 байт, WS использует прежний RX после полного Close frame,
без decoder/callbacks. Новых буферов, retained storage и allocations не добавлено.
Peer, ожидающий TCP EOF, может занять весь bounded drain; затем сервер сам закрывает
соединение. Поздние данные или превышение бюджета всё ещё могут вызвать reset;
доставка Close остаётся best effort. Deadline проверяется перед I/O, а фактический
release зависит от reactor/scheduler/SDK. C3 connection sizeof1008 B — прежняя
compiler-only оценка (+16 к P4), а не новое измерение heap/high-water.

## SSE handoff и restart

`sse::TcpAdapter` использует canonical QueuedAdapter/Sessions/encoder. Reserve не
отправляет и не закрывает socket; rollback возвращает исходное HTTP-владение.
Activation подтверждает owned lease, но worker не выполняет native I/O/close до
возврата HTTP callback. Request/Response/Next и parser workspace не сохраняются.

`opened.token` можно сохранить как значение в синхронизированном состоянии
приложения. Frame копируется в bounded очередь через `events.publish`; Queued не
означает доставку. Defaults SSE отдельные:4 slots,3 pending/client,768 encoded bytes,
2304 queued bytes/client и8192 total. Лимиты WS не изменяют эти SSE budgets.

Completion учитывается только после фактического освобождения underlying lease.
SSE cleanup вызывает registry close без отдельного SHUT_WR и без нового RX drain:
входной HTTP request уже прочитан, произвольный post-open RX/pipelining не поддерживается.
Поздний RX может привести к reset; Queued не обещает доставку.
Retained native close failure оставляет сессию активной и drain неуспешным;
`close_errors()` позволяет видеть ошибки, а не обещает исправить ambiguous Fatal.
Только известный retained WouldBlock допускает одну новую попытку на явный stop.
После успешного drain тот же TcpAdapter/EventSource можно запустить вновь, включая
цикл end/begin HTTP backend. Generations не сбрасываются; прежние tokens остаются
Closed. Это TCP opt-in: прежние Memory/Windows/IDF lifecycle этим не меняются.

`examples::SseTest` — диагностическое приложение, не обязательная часть EventSource.
Его initial connected-event при Busy удерживается как bounded owned metadata и
повторяется из `SseTest::poll()` в application owner loop вне HTTP callbacks.
Host/minimal owner loops вызывают этот poll и прекращают его до drain.
Обычный Publish409 с exact body `busy` означает, что никто не принял payload;
browser fixture делает ограниченный retry только такого ответа. Queued/HTTP200
не повторяется. Core SSE Busy/queue policy от этого не меняется.

## Историческая SDK build matrix и текущие MVP-сборки

До safer-close для C3 собраны HTTP-only, WS без SSE, SSE без WS и WS+SSE для двух
backend и двух точных SDK:16 профилей. Эта таблица — исторический checkpoint,
а не повторная проверка всех профилей текущего кода. Это одно минимальное приложение без Wi-Fi setup, UI assets
и больших unit fixtures, с одинаковыми routes/handlers и custom partition table.
Оно отличается от полного diagnostic sample, используемого на плате.

SDK5.4.0: PlatformIO espressif32 6.10.0, riscv32 GCC14.2.0_20241119/newlib.
SDK6.0.3: официальный exact SDK через idf.py, GCC15.2.0_20251204/picolibc.
Его фактическая строка версии — `v6.0.3-dirty`: Windows extraction меняет executable
bits; проверка archive/source provenance описана в отчёте. SDK6.1 не подменяет эту
строку. Для component/main проверены GNU++17, `-Og`, exceptions/RTTI off; SDK6
default GNU++26 перекрывается явным GNU++17. Компонент задаёт этот standard для
своих C++ sources; в сравниваемом main он также задан явно.

| SDK | Backend | Возможности | Flash, B | Static RAM, B | App image, B |
|---|---|---|---:|---:|---:|
|5.4.0|HTTPD|HTTP|368356|12040|368752|
|5.4.0|TCP|HTTP|335954|12064|336352|
|5.4.0|HTTPD|HTTP+WS|398764|12208|409920|
|5.4.0|TCP|HTTP+WS|353616|12160|354016|
|5.4.0|HTTPD|HTTP+SSE|378634|25064|379040|
|5.4.0|TCP|HTTP+SSE|349710|25088|350112|
|5.4.0|HTTPD|HTTP+WS+SSE|409048|25216|419696|
|5.4.0|TCP|HTTP+WS+SSE|366744|25184|367136|
|6.0.3|HTTPD|HTTP|317180|13888|317856|
|6.0.3|TCP|HTTP|293658|13916|294320|
|6.0.3|HTTPD|HTTP+WS, post-hook y|355706|14064|356368|
|6.0.3|TCP|HTTP+WS|310986|14012|311648|
|6.0.3|HTTPD|HTTP+SSE|327566|26912|328240|
|6.0.3|TCP|HTTP+SSE|307120|26940|307776|
|6.0.3|HTTPD|HTTP+WS+SSE, post-hook y|366084|27080|366752|
|6.0.3|TCP|HTTP+WS+SSE|323750|27044|324416|

Две native IDF6 WS строки обновлены после обнаруженного аппаратным запуском
отсутствия opening callback при выключенном SDK post-handshake option: успешный
link не доказал runtime. После исправления регистрации и включения обязательного
флага оба профиля повторно собраны:181.26s/186.06s, Flash+152 B каждый,
static RAM без изменения. Предыдущие16 ELF/bin/map/config наборов сохранены как
pre-fix evidence. Новая сборка сама по себе не закрывает аппаратную проверку SDK6.
Остальные6 native-профилей также повторно собраны с окончательным исходником;
их размеры не изменились. TCP-профили этот native source не компилируют,
на том checkpoint их исходники оставались прежними.

После safer-close свежие SDK compile/link выполнены последовательно только для
двух полных TCP MVP-профилей HTTP+WS+SSE: IDF5.4.0 и IDF6.0.3, 2/2 PASS.
Здесь используется полный minimal diagnostic sample с Wi-Fi, UI и встроенными
функциональными проверками, поэтому его размеры нельзя напрямую сравнивать с lean
таблицей выше. Проверены current root sources без witness copy/wrappers,
GNU++17, exceptions/RTTI off и отсутствие HTTPD/esp-tls в обоих графах.

| Текущий полный TCP sample | Static DRAM data+bss, B | App image, B |
|---|---:|---:|
|IDF5.4.0, HTTP+WS+SSE|46968|924448|
|IDF6.0.3, HTTP+WS+SSE|49244|939456|

Это static DRAM, не runtime heap/stack. Safer-close не добавляет буферов или очередей;
успешная SDK-сборка не заменяет аппаратную приёмку и не обновляет автоматически
16 исторических строк.

В исторической lean-таблице `Flash` — одинаковая для обоих SDK сумма
`.flash.text + .flash.rodata + .iram0.text + .dram0.data`, соответствующая SDK5 PIO metric.
Static RAM — `.dram0.data + .dram0.bss`. App image учитывает иной segment layout/padding; для partition
проверяется именно image и его write/erase ranges. Эти столбцы не включают весь
runtime heap, task stacks и socket memory. Все числа точные в bytes;1 KiB=1024 B.

В HTTP-only TCP меньше HTTPD на32402 B (31.6426 KiB) в SDK5 и23522 B (22.9707 KiB)
в SDK6. SDK5 map показывает удаление http_parser/HTTPD и части libc formatting,
добавление TCP runtime/wire и изменение merged string pool/fill. Приложение,
которому самому нужен snprintf, может сохранить эти libc objects; этот выигрыш
не гарантируется каждой прошивке. Разницу между SDK нельзя приписывать движку:
меняются compiler, libc и SDK sources. Подробный object/section ledger — в отчёте.

WS включает codec/handshake/session/TX-pool code либо SDK HTTPD WS/native binding;
SSE включает canonical encoder/Sessions и конкретный adapter/worker. SSE прибавляет
13024 B (12.71875 KiB) static RAM к HTTP-only в обоих backend/SDK. Из них12288 B —
прежние payload-массивы Frame:4 clients × (header +3 pending) ×768 B; остальное —
slot/adapter/route metadata. Это не новый дополнительный TCP SSE pool.

TCP HTTP body workspace резервируется один раз до4096 B (4 KiB), head —8192 B
(8 KiB); WS TX payload pool default16 KiB учитывается отдельно. Private TCP/IDF worker
по умолчанию запрашивает6144 B (6 KiB) stack для HTTP и4096 B (4 KiB) для SSE, плюс Worker/TCB
и SDK bookkeeping. Compiler TLS занимает часть каждого task stack, а не один общий
global buffer. Static/compiler sizes не заменяют фактический heap/stack high-water
и не доказывают отсутствие leak или достаточный запас для любого callback.

Для ESP-IDF размер HTTP worker задаётся `CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE`
(4096–65536 байт, default6144). Профиль `touch_lcd7/s3_tcp` использует8192:
при6144 запрос INFO на реальной S3 вызвал stack overflow задачи `naw_tcp_http`.
Профиль `c6_tcp` сохраняет6144. Настройка не меняет HTTPD или host threads.

Фактические graphs/ELF всех TCP-профилей не требуют HTTPD/esp-tls; HTTP/WS-only
не компилируют SSE encoder. Дополнительно проверены4 actual SDK6 configure cases
приоритета config/defaults/target sidecars. Host CMake regression проверяет те же
контракты выбора через mocks; она не подменяет SDK configure/link/runtime.

## Текущие наблюдения C3 и аппаратные границы

Для текущих safer-close TCP-сборок разрешённой C3 на IDF5.4.0 и IDF6.0.3 получены
PASS: on-device response 89 / loopback 63 / lifecycle 30, LAN HTTP/WS, response probe
из 78 проверок с двумя клиентами, SSE admission с тремя streams и stop с двумя streams
при WS witness. Сборки и эти конкретные runtime-проверки учитываются отдельно.

Обычный browser-прогон SDK5 удерживал один постоянный WS и два SSE-потока на
20 итерациях: каждый SSE получил ровно 20 событий, совпали 40 WS echo, после drain
завершены ровно два SSE-потока. Аналогичный обычный steady-прогон текущего SDK6
тоже прошёл: 20 health-проверок, 20 text и 20 binary WS echo, 20 публикаций и по 20
доставленных событий каждому SSE-потоку. От baseline 0/0 достигнуты begun=completed=2,
active=queued=0 после одной попытки cleanup. Длительность steady-фазы — 7725 ms,
всего — 8968 ms; serial observer не зафиксировал panic. Сам EventSource остаётся
запущенным. Это отдельный MVP-прогон: более ранний повторный coexistence-прогон
полного UI завершился WS timeout и не переобъявляется успешным. Исторические native
HTTPD и прежние TCP browser результаты относятся к своим ревизиям в поэтапном отчёте.

Ограниченный MVP C3 на двух проверенных SDK подтверждён. Это не закрытие полного
P6, не приёмка четырёх клиентов, регрессии полного UI или нагрузочной/длительной
программы P7. Актуальные статусы и ограничения вести в поэтапном отчёте.

Browser clients, HTTP connections, WS/SSE clients, SDK sockets и TCP PCBs — разные
budgets. В текущем C3 стенде оставленная открытой owner Chrome page занимала3
соединения;2 SSE +1 WS +1 control дают ещё4, то есть7 client connections до учёта
listener/служебных SDK ресурсов. Не закрывать owner page ради успешного теста и не
считать browser tab одним сокетом. Backlog и `max_websocket_clients` не заменяют
`HttpLimits::max_connections` и SDK socket/PCB limits.

Сборки SDK выполняются в фоне; аппаратные действия координируются отдельно.
Перед каждой записью повторно идентифицировать разрешённую C3, текущий port/chip/MAC
и проверить image, действующую partition table и sector-rounded write ranges.
Текущий проверенный app-only профиль: app0 от0x10000, размер не более0x280000,
конец0x290000. Эти числа не переносятся на другую partition table. Команда прошивки
не должна включать bootloader/partition/NVS; не менять Wi-Fi secrets ради выбора backend.
Без flash backup и отдельных post-flash readback/hash passes; штатная vendor write
проверка сохраняется. Аппаратный порядок и границы соседнего Linux/ioBroker
процесса приведены в [плане/runbook](plans/PLAN_20261024_nawserver_tcp-engine-implementation.md).
Stress/fuzz/forced-OOM остаются на изолированном стенде; normal-load и soak ещё
требуют отдельного отчёта с реальными возможностями МК.

## Host targets

`host/CMakeLists.txt` содержит общие `nawserver_http_wire`, `nawserver_tcp_session`,
`nawserver_websocket_wire`, `nawserver_tcp_protocol`, `nawserver_tcp` и platform
`nawserver_tcp_windows` / `nawserver_tcp_linux`: runtime/worker и concrete transport.
Linux использует private pthread worker, poll, nonblocking/CLOEXEC sockets и
MSG_NOSIGNAL; process SIGPIPE policy не изменяется. Общие sources C++17 компилируются
без Windows/IDF headers, exceptions и RTTI. Private Windows worker использует
свой файл platform glue. Это не публичная Task/Clock/HAL abstraction.

`nawserver_tcp_demo [port] [duration_ms] [numeric_ipv4_bind]` запускает HTTP/WS/SSE mini UI;
defaults127.0.0.1:8081. Указанная duration ограничивает время процесса и завершает его
через SSE drain, server.end и transport.stop. Root frontend используется без копирования;
маршруты/auth/handlers общие с прежним demo через `host/http_demo.h`.

Полная host-регрессия собирается из `tests/CMakeLists.txt`; запускать CTest из
build directory. SDK stubs проверяют native lifecycle и публичную IDF socket
границу на host; это не runtime на C3. C3 object-only compile также не заменяет
SDK link/железо.

Native Linux build: `cmake -S tests -B local/linux -G Ninja -DCMAKE_CXX_COMPILER=g++`,
затем `cmake --build local/linux` и `ctest --test-dir local/linux --output-on-failure`.
Для high-fd test нужен process nofile limit≥1100; фактический профиль использовал4096.
Clang wire fuzz targets включаются явно через `NAWSERVER_BUILD_WIRE_FUZZERS=ON`;
seed corpus генерирует `tests/fuzz/generate_corpus.go`. Fuzz/stress/OOM запускаются
в isolated environment, не на production ioBroker. Read-only resource observer:
`tools/tcp_resource_probe`, пример/ограничения — в source и поэтапном отчёте.

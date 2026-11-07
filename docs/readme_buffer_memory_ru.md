# Память и пределы ресурсов NaWServer

Настройки принадлежат публичной библиотеке; демо только выбирают значения.

| Группа | Настройка | Что входит |
|---|---|---|
| TCP HTTP/WS | `TcpOptions.buffer_memory` | HTTP header/body, массивы таблиц соединений, WS receive/metadata/pending, общий TX pool |
| SSE | `sse::Limits.buffer_memory` | Очереди, текущие кадры и заголовки только выделенных SSE-клиентов |
| JSON/MCP | Caller-owned workspace/output | Размещение определяет приложение |

`max_bytes` — верхний предел суммы реально выделенных массивов, включая
удерживаемый начальный резерв и небольшие таблицы до максимума слотов.
Сам предел не выделяет память. Это **не** вся RAM сервера:
allocator overhead, небольшие control objects/shared_ptr control blocks,
стеки, HTTP Request/Response и память SDK/ОС отдельно. Число таких объектов
ограничивается connection/client/message limits.

## ESP-IDF

```cpp
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/backends/esp_idf_memory.h>

NaWServer::TcpOptions options;
options.buffer_memory = {
    NaWServer::espIdfBufferAllocator(NaWServer::EspIdfMemoryPolicy::PsramOnly),
    64 * 1024,
    32 * 1024 // Leave this much free in the selected heap before any new allocation.
};
options.http_memory.enabled = true;
options.http_memory.initial_requests = 2;
options.http_memory.maximum_requests = 9;
options.http_memory.initial_websocket_clients = 0;
options.http_memory.reserved_requests = 0;
options.keep_alive.enabled = true;
NaWServer::TcpHttpBackend backend(transport, options);

NaWServer::sse::Limits events;
events.buffer_memory = {
    NaWServer::espIdfBufferAllocator(NaWServer::EspIdfMemoryPolicy::PreferPsram),
    16 * 1024
};
NaWServer::sse::TcpAdapter event_backend(backend, events);
```

| Policy | Поведение |
|---|---|
| `System` | Обычный allocator платформы |
| `InternalOnly` | Только internal RAM; отказ при нехватке |
| `PsramOnly` | Только PSRAM; отказ при отсутствии/нехватке |
| `PreferInternal` | Сначала internal, разрешён fallback в PSRAM |
| `PreferPsram` | Сначала PSRAM, разрешён fallback в internal |

Kconfig defaults: `CONFIG_NAWSERVER_TCP_MEMORY_POLICY` и
`CONFIG_NAWSERVER_SSE_MEMORY_POLICY` (0–4 в порядке таблицы);
`CONFIG_NAWSERVER_TCP_BUFFER_BYTES=65536`,
`CONFIG_NAWSERVER_SSE_BUFFER_BYTES=16384`.
Начальная политика — 0/system. Явные параметры экземпляра имеют приоритет.
Изменять настройки работающего backend нельзя.

ESP-IDF capability malloc гарантирует только четырёхбайтовое выравнивание.
Allocator библиотеки обеспечивает `max_align_t` через ограниченный padding и
сохраняет исходный указатель для release. Этот overhead относится к allocator,
не к `max_bytes`. System передаёт SDK полный размер с padding: у пограничного
16-КиБ пула это может переключить выбор обычного malloc в PSRAM при пороге
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`. Поэтому System не означает «всё в SRAM».
Буферы этого allocator освобождаются его callback, не прямым `heap_caps_free`.

Строгий PSRAM на МК без PSRAM не запускается. Предпочтительный вариант допускает
fallback и поэтому не гарантирует сохранение internal RAM. Настройки не меняют
глобальные malloc, Wi-Fi/lwIP, task stacks и native HTTPD buffers.

## Другие платформы и собственный allocator

`BufferAllocator` содержит context и две non-throwing функции allocate/release.
Дополнительно возможны `available` и `allocate_guarded` для контроля свободной
памяти. `min_free_bytes=0` отключает этот контроль; ненулевое значение требует
`available`, иначе старт отклоняется. Windows/Linux по умолчанию используют
обычный allocator и byte budget без предположений о «свободной RAM всей ОС».
Приложение может передать свой allocator/probe; ESP-IDF helper уже содержит их.
Они должны быть thread-safe и возвращать max_align_t-aligned память; context
живёт дольше всех буферов, включая сохранённые закрытые WS handles.
Пустой allocator выбирает default платформы. ESP-IDF заголовки общему коду не нужны.
Конструкторы не выделяют крупные буферы; штатное создание — при start.

`bufferMemoryStats()` на TCP backend и queued SSE adapters возвращает used/peak,
предел, отказы бюджета и неудачные выделения. Проверка бюджета предшествует
allocator; отказ allocator откатывает резерв. TCP статистику читать при
сериализованном lifecycle, не одновременно с start/stop. Пиковые значения
относятся к последнему запуску. После успешного освобождения used становится 0.

## Начальный резерв, динамика и ранние флаги

`memory_manager.h` содержит небольшой общий `MemoryManager`, `MemoryBlock` и
`MemoryPoolOptions {initial, maximum, block_bytes}`. Контракт: `initial` блоков
полного размера при старте; следующие блоки — только по запросу, фактического
нужного размера до `block_bytes`; при возврате динамический блок сразу
освобождается, начальный остаётся доступным для повторного использования.
Дескрипторы до `maximum` небольшие и выделяются при старте. Владение — move-only
lease; менеджер обязан жить дольше всех выданных блоков.

| Параметр TCP | Смысл |
|---|---|
| `http_memory.enabled` | Включить управляемый путь без обязательной политики Admission |
| `initial_requests=2`, `maximum_requests=9` | Два начальных места для metadata/ответов, остальные динамические. С admission максимум должен вмещать сумму HTTP-квот |
| `reserved_requests=0` | Нет отдельного физического резерва основного ключа; логическая квота может оставаться включённой |
| `reserved_requests>0` | Указанное число HTTP-мест получает защищённые начальные metadata/body/response; policy передаёт только признак защищённого разрешения. Входят в `initial_requests`, не добавляются поверх него |
| `initial_websocket_clients=0` | До upgrade WS arena отсутствует; при закрытии динамическая арена возвращается |
| `buffer_memory.max_bytes` | Общий потолок реально выделенных TCP буферов/таблиц |
| `buffer_memory.min_free_bytes` | Остаток свободной памяти выбранного allocator после нового выделения |
| `sse::Limits.initial_clients=0` | SSE frame sets только по фактическому подключению; динамические освобождаются при завершении |

GET без тела не выделяет body block. Общие входные тела всегда динамические;
исключение — явно включённое начальное тело основного API. Фиксированный блок
может выдаваться и при низкой памяти: это повторное использование, не выделение.
При нехватке нельзя отбирать уже выданные блоки у работающего клиента.

`MemoryManager.readiness(bytes)` даёт предварительное решение для конкретного
размера, `MemoryPoolStats.blocked` сохраняет причину: `Ready`, `ByteLimit`,
`LowMemory`, `AllocationFailure`, `SlotLimit`. Отказы/пики/текущая занятость
доступны через `TcpHttpBackend.managedMemoryStats()` и `sse::AdapterStats.memory`.
Это снимок, не вечная защёлка: последующая проверка/освобождение обновляет флаг.
Если настроенный probe не смог получить показания, новые выделения запрещены.
Флаг root budget и флаг конкретного пула могут различаться по требуемому размеру.
Проверка бюджета и выбранной heap выполняется до allocator; краткий503 использует
уже имеющуюся служебную память, без дополнительного response block.

ESP-IDF strict policy проверяет конкретную heap, включая alignment overhead;
preferred policy проверяет кандидатов по очереди. `System` использует агрегат
`MALLOC_CAP_8BIT`, который на плате с PSRAM не означает именно внутреннюю RAM.
Для порога internal выбирать `InternalOnly`. Порог не ограничивает независимые
выделения Wi-Fi/lwIP/LCD/других задач и не гарантирует глобальный остаток после
их конкурентной работы. Размеры блоков остаются ограниченными и при наличии PSRAM.

## Владение ответом и keep-alive

`Response.send()` копирует изменяемый ответ до возврата callback: его исходный
буфер часто общий у приложения. Копия теперь выделяется только при необходимости.
`sendImmutable()` заимствует неизменяемые данные до stop, например flash assets.
`sendOwned(status, type, MemoryBlock, used)` передаёт move-владение блоку до конца
отправки/отмены, без второй payload-копии в управляемом TCP пути. Legacy/native
backend может потребить этот блок синхронно. Ни один вариант не обещает отсутствие
копий внутри SDK/ОС. Заголовки запроса копируются из ingress один раз: это позволяет
освободить маленькое входное место для следующего клиента и сохранить срок жизни.

`keep_alive` по умолчанию выключен; включение использует управляемый путь даже
без admission. HTTP/1.1, последовательные запросы: `maximum_idle=4`,
`idle_timeout_ms=5000`, `maximum_requests=100` на соединение по умолчанию.
После ответа body/metadata/dynamic response и квота HTTP освобождаются;
до следующего запроса остаются native socket и маленький idle descriptor.
Каждый следующий запрос снова проходит лимиты/проверку ключа. При физическом
резерве API одно idle-место оставлено основному ключу. `Connection: close`,
лимит числа запросов/простоя, ошибка и stop закрывают соединение. Полный pipeline
не поддерживается; WS/SSE остаются отдельными длительными соединениями.
Количество native sockets/lwIP buffers нужно рассчитывать с учётом idle-мест.

В текущей сборке управляемый путь исключается тем же compile-time флагом
`CONFIG_NAWSERVER_TCP_MANAGED` / `NAWSERVER_HOST_TCP_MANAGED`;
это не обязывает подключать policy. Запрос функции при выключенном коде
возвращает Unsupported. Без включения managed/keep-alive сохраняется
прежний одиночный workspace/close путь. Настройки экземпляра можно задавать
из своих `#define` и статической инициализации, без runtime-конфигуратора.

Замеры конкретных плат и полная конфигурация: [27.10.2026](readme_20261027_memory_manager_keepalive_ru.md).

## Жёсткие пределы

| Параметр | Реакция |
|---|---|
| `HttpLimits.max_connections` | Лишнее соединение не получает session slot |
| `max_header_bytes`, `max_headers`, `max_target_size`, `max_body_size` | Ошибка ограничений HTTP до обработки приложения |
| `WebSocketLimits.max_clients`, `TcpOptions.max_websocket_clients` | Отказ upgrade |
| `WebSocketLimits.max_message_size` | Отказ слишком большому сообщению |
| `max_pending_messages`, новый `max_pending_bytes` | Отказ очереди; TCP запрашивает закрытие перегруженной WS-сессии |
| `websocket_tx_blocks`, `websocket_tx_block_size` | Фиксированный общий TX pool; GlobalFull не расширяет его |
| `sse::Limits.clients/pending/bytes_per_client/total_bytes` | Отказ новому клиенту/событию; индивидуальный overflow закрывает сессию |
| TCP exchange/no-progress, WS send, SSE transfer/stall deadlines | Ограничение зависших операций с освобождением при закрытии |
| `TcpOptions.close_timeout_ms=250`, `close_drain_bytes=8192` | Ограниченный слив HTTP/WS перед закрытием: время и число отброшенных входных байтов; не выделяет буфер такого размера |
| `buffer_memory.max_bytes` | Отказ до выделения превышающего бюджет массива |

Параметры закрытия общие для TCP backend на всех платформах и задаются через
`TcpOptions` в приложении. Нулевые значения отклоняются при старте. HTTP grace
также ограничен оставшимся временем exchange/no-progress; WS использует
неблокирующий слив по одному чтению за событие, без callbacks приложения.
Это максимальное окно до native close, а не гарантия доставки Close при
бесконечном входном потоке или потере сети. Системные TCP buffers и состояния
после native close принадлежат ОС/lwIP и не входят в `buffer_memory.max_bytes`.

WS pending bytes включают активный частично переданный payload. Занятый общий
пул не объявляется виной одного клиента. При отказе старта TCP возвращает
`TransportError`; memory stats различают budget rejection и allocator failure.
Неудачный stop с `CleanupRequired` сохраняет ресурсы — их нельзя освобождать
раньше завершения владельца ввода-вывода.

## Замеры RAM

`heap_caps_get_minimum_free_size(INTERNAL|8BIT)` суммирует исторические минимумы
разных heap regions. Они могли наступить в разное время: число 96 Б из ночного
журнала **не доказывает одновременный остаток 96 Б всей internal RAM**.
Минимум текущих free в сохранённых ночных снимках S3 — 6663 Б, largest block1536 Б.
При сравнении отдельно фиксировать sampled free/blocks, агрегированный SDK low
watermark, PSRAM, стеки, ELF/config, профиль и фоновые подключения.

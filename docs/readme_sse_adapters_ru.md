# SSE: общая часть и адаптеры — 24 октября 2026

> Исторический отчёт о версии 24.10.2026. С 03.11.2026 нативный ESP-IDF HTTPD
> удалён. Его типы, lifecycle и приведённые ниже проверки относятся только к
> указанному старому checkpoint; это не инструкции для текущей сборки.
>
> Актуально: ESP-IDF, Windows, Linux и RP используют `TcpHttpBackend` с адаптером
> sockets своей платформы. Маршруты объявляются через `routes()`, ранние
> auth/quota middleware выполняются до тела и upgrade. `Drop` передаёт отмену
> владельцу I/O; WS/SSE сохраняют lease ресурса до закрытия соединения.
> SSE подключается как `sse::TcpAdapter(http_backend)`; native HTTPD async API нет.
> См. [переносимую сборку](readme_portable_build_ru.md),
> [текущий SSE API](readme_sse_ru.md) и
> [общую очередь SSE](readme_sse_shared_transmission_ru.md).

`EventSource` больше не наследуется от IDF. Он заимствует явно переданный
`sse::Backend`; публичные sse.h/sse_backend.h собираются без SDK headers.
Выбор платформы виден в статической композиции приложения:

```cpp
#include <NaWServer/sse.h>
#include <NaWServer/backends/esp_idf_sse.h>
static NaWServer::sse::IdfAdapter backend;
static NaWServer::sse::EventSource events(backend);
```

Windows выбирает `sse::WindowsAdapter` из backends/windows_sse.h;
тесты — `sse::MemoryAdapter` из backends/memory_sse.h. Конструкторы не запускают
задачи/сокеты. После prerequisites приложение вызывает events.start().
Существующие примеры мигрированы только в include и конструкции SSE backend;
их продуктовая логика не менялась. Старый неявный выбор IDF не сохранён.

## Граница и владение

| Общая часть | Адаптер |
|---|---|
| EventSource: open/publish/close/stats/start/stop | Выполнение lifecycle на платформе |
| Encoder и bounded Sessions | Выполнение write/close шагов |
| Response bridge и owned HTTP preface | Получение owned transport при reserve |
| Backend::Reservation с автоматическим rollback | Захват/возврат нативного соединения |
| Token/generation, queue limits, heartbeat/stall/deadline | Время, worker и неблокирующий сетевой ввод-вывод |

Порядок прежний: auth/policy → resolveHeaders → owned preface → reserve →
claimExternal → activate. До claim SSE bytes не отправляются. Rollback возвращает
владение исходному callback и позволяет обычный HTTP-ответ. Ошибка/вторая отправка
после External запрашивает закрытие потока, не отправляет второй ответ.
Request/Response/Next/прикладные callbacks не удерживаются.

Backend — виртуальный SSE-контракт, без общей RTOS/HAL-обёртки. Общий Sessions
применяется всеми тремя реализациями. IDF сохраняет свой прежний runtime;
Memory/Windows используют общий QueuedAdapter с owned Stream и одним write-step
не более256 bytes на слот за проход. Queue policy/encoding не дублируются.

IDF использует native async_handler_begin/complete, lwIP MSG_DONTWAIT,
esp_timer и один FreeRTOS worker. При close сначала shutdown, затем complete;
fd-only deferred close после потери владения не вводится. SDK типы находятся за
backends/esp_idf_sse.h / detail/sse/adapter.hpp и в src/sse/adapter.cpp.

Windows использует nonblocking Winsock и один SSE worker. HTTP worker передаёт
socket только при activate; close сохраняет его до завершения исходного callback,
чтобы не закрыть повторно использованный descriptor. HTTP backend учитывает SSE
в допуске соединений и отклоняет end при живых SSE registrations. Владелец сначала
успешно events.stop(timeout), затем server.end(); неудачный stop/end требует retry
с сохранением объектов. SSE backend связан с одним сериализованным HTTP backend.

Memory передаёт обычные запросы через HttpServer/Route и тот же EventSource.
MemoryResponse::sse_stream даёт bounded capture, take(), setMaxWrite(0) для stall,
частичную запись, failWrites и счётчики finish/rollback. MemoryAdapter::advance(ms)
продвигает ручное время и выполняет один проход. Сеть/RTOS не эмулируются.

Один backend имеет один успешный lifecycle, как прежний IDF SSE. После stop нужен
новый экземпляр; простое повторение start не сбрасывает generation/state.
Лимиты прежние:4 клиента,3 pending/client,768 bytes/Frame,2304 bytes/client,
8192 queued total; heartbeat15s, stall2s, transfer5s. Limits могут уменьшаться.
Индивидуальный overflow/stall закрывает сессию; GlobalFull/Busy — явные отказы.
Queued означает копирование в очередь, не доставку браузеру.

Plain HTTP; TLS/send overrides/external forced close активных async requests на
IDF остаются неподдержанными. Не добавлены replay, history, worker pools или codec.
Прочие результаты аудита переносимости — Server facade, ssize_t, build packaging,
требования C++ runtime — этим этапом не менялись.

## Проверки

- Host CTest18/18 PASS, включая прежние HTTP/WS/SSE encoder/bridge проверки.
- Memory SSE68 checks: auth доhandoff, Last-Event-ID absent/empty/128/129,
  UTF-8/multiline/NUL, headers, copied queue, limits, stale tokens, stall/failure,
  rollback, abort послеclaim и stop.
- Windows SSE socket test: scoped reservation → ordinary409; duplicate send после
  claim → закрытие без второго ответа; HTTP и WS рядом с SSE; stop/refusal/drain.
- Edge на Windows и C3:18/18 и повтор PASS, включая настоящий EventSource,
  UTF-8/multiline/trailing LF, empty data/id reset, NUL, HTTP/WS coexistence,
  auth401 доopen. Общий HTML/CSS/JS, CSS отдельным ресурсом; light/dark,
  viewport430/1180px проверены по скриншотам.
- Pinned minimal/C3, ESP-IDF5.4.0, GCC14.2 C++17: build PASS в фоне, записан
  только app0@0x10000, final image943600 bytes в пределах0x280000. Native startup:
  response/basic89, loopback63/63 и lifecycle27/27; LANHTTP/WS smoke и198 HTTP checks PASS.

Первый C3 запуск выявил watchdog до Wi-Fi: при CONFIG_FREERTOS_HZ=100 старое
pdMS_TO_TICKS(1) равно0, worker priority4 вытеснял app_main. SSE runtime исправлен
на vTaskDelay(1), минимум один RTOS tick. Конфигурация FreeRTOS не менялась.
Повторный startup и настоящие SSE browser checks прошли без watchdog.

Прогон четырёх подписчиков выявил ещё один native случай: после36 checks пятая
попытка и новые HTTP connections зависали. HTTPD исключает async sockets из select,
а при заполненных slots перестаёт слушать listener; async_handler_complete только
снимает флаг, но не пробуждает select. SSE worker теперь после batch completion
подаёт stateless refresh через штатный httpd_queue_work. В deferred work нет fd,
payload или указателя на adapter. При отказе enqueue worker повторяет refresh;
успешный stop ждёт его принятия. Лимиты connections и LRU policy не менялись.

После исправления один Go-клиент прошёл на Windows и C3:48 checks — auth,
Last-Event-ID absent/empty/128/129, GET body rejection,4 подписчика и503 пятому,
UTF-8/multiline/trailing LF/NUL, id reset, HTTP health рядом с SSE, EOF и reconnect.
В конце active=0, begun==completed, queued_bytes=0. Отдельный owner stop с четырьмя
активными streams:22 checks PASS на каждой платформе; Windows47/47 и C3 13/13
begun/completed, queued0. После stop оба демо восстановлены: процесс Windows
перезапущен, C3 выполнен reset той же прошивки, без новой записи.
Ранние неуспешные прогоны и диагностические логи сохранены в ignored local/.
После финальных browser/LAN проверок C3: free heap178676, minimum155580,
largest block114688 bytes; HTTP task minimum free stack1304 bytes при stack6144.
Это короткий снимок с включённым SSE runtime, не доказательство отсутствия утечек.

Скрипты: tests/memory_sse_test.cpp, tests/windows_sse_test.cpp,
tests/browser_http_check.cjs, tests/sse_smoke/main.go. Mini UI/diagnostic endpoints
общие для Windows/C3: examples/common/sse_test.hpp. Stop endpoint только выставляет
флаг; фактический drain выполняет owner task вне HTTP callback.
Raw serial, адрес/device identity, firmware, screenshots и JSON — только ignored
local/http-host-build/ и local/idf-c3-hardware/. Это короткая acceptance, не soak,
не WCET и не доказательство поддержки других SoC или всех MCU toolchains.

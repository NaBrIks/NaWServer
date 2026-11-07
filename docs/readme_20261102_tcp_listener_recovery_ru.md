# TCP listener: восстановление и диагностика, 02.11.2026

Руководство описывает публичный `TcpHttpBackend`, без зависимости от приложения,
Wi-Fi manager, SSID, ESP-событий или демо.

## Причина и поведение

Раньше fatal wait, error readiness listener или fatal accept завершали HTTP worker.
Listener закрывался, но `runtime_` оставался до `stop()`: новый `start()` возвращал
`InvalidState`, а `HttpServer::running()` продолжал отражать успешный первоначальный
старт. Диагностика различает эту ошибку backend и внешнюю недоступность сети.

| Событие | Теперь |
|---|---|
| Fatal wait / ошибка listener / fatal accept без удержанного accepted handle | Записать причину и native error, закрыть listener, затем переоткрывать с паузой |
| Повторный open не удался | Сохранить ошибку, следующая попытка через заданную паузу |
| `WouldBlock`/`Interrupted` от accept, timeout/interrupted wait | Штатное продолжение без восстановления и счётчика ошибки |
| Listener close вернул документированный retained `WouldBlock` | Повторять close с той же паузой; новый listener до освобождения не открывать |
| Неоднозначный retained close / неочищенный accepted handle | `CleanupRequired`, владение сохранено; нет слепого close/reopen |
| Первоначальная ошибка `begin()` | Синхронная ошибка; приложение завершает необходимую очистку и решает, когда повторять запуск |

Listener восстанавливается на исходном bind address и том же фактически выделенном
порту, включая первоначальный `port=0`. Другой процесс может занять порт во время
паузы: тогда продолжаются ограниченные по частоте попытки, без захвата чужих handles.
При отдельной ошибке listener действующие WS и managed HTTP jobs сохраняют владельцев.
При ошибке всего wait-набора нельзя надёжно определить плохой WS descriptor:
worker закрывает свои WS-соединения перед восстановлением. Внешний SSE-владелец
не подменяется. Это восстановление доступности listener, а не обещание сохранения
соединений через потерю сети, доставки ответа или автоматического перезапуска Wi-Fi.

## Настройки и API

```cpp
NaWServer::TcpOptions options;
options.recovery.enabled = true;          // default: true
options.recovery.retry_delay_ms = 1000;   // default: 250 ms, must be positive when enabled
NaWServer::TcpHttpBackend backend(transport, options);

const NaWServer::TcpDiagnostics snapshot = backend.diagnostics();
```

`diagnostics()` копирует небольшой снимок под отдельным mutex, без удержания lock
во время transport I/O, пользовательского callback или ожидания worker. Метод можно
вызывать параллельно с worker и owner `start()/stop()`, пока жив сам backend.
Lifecycle-вызовы по-прежнему сериализует приложение; остальные stats API этим
изменением не получают нового контракта потокобезопасности. Новых payload-буферов
или выделений на каждое чтение диагностики нет.

| Поле | Значение |
|---|---|
| `state` | `Stopped`, `Starting`, `Listening`, `Recovering`, `Stopping`, `CleanupRequired`, `Failed` |
| `last_failure` | `None`, `Storage`, `Listen`, `Wait`, `Listener`, `Accept`, `ConnectionCleanup`, `ListenerCleanup`, `WorkerStart` |
| `native_error` | Код последней ошибки платформы; 0, если конкретный код недоступен |
| `port` | Текущий порт listener; 0 во время переоткрытия и после успешного stop |
| `errors` | Число зарегистрированных ошибок, включая неудачные reopen/close |
| `retries` | Число повторных попыток open listener; повтор close сюда не входит |
| `recoveries` | Число успешных reopen |

Счётчики насыщаются на `UINT32_MAX`. Последняя ошибка и счётчики сохраняются после
восстановления и успешного stop; прошедшая валидацию новая попытка start их сбрасывает.
`HttpServer::running()` сохраняет прежний lifecycle-контракт: текущую работоспособность
listener оценивают по `diagnostics().state`, а не только по факту принятого `begin()`.

При отсутствии listener worker спит положительными интервалами не более 20 ms.
Stop прерывает даже длительную паузу восстановления; лимиты времени transport и
пользовательского callback при этом остаются прежними. Это не жёсткая гарантия
реального времени для системного close/open или произвольного обработчика приложения.
Есть только два параметра восстановления, без внешнего policy manager и ESP-условий.

`Transport::listenerError(Handle)` — необязательная диагностическая операция,
по умолчанию возвращающая 0. Windows/Linux/IDF читают socket error только для
ошибочного listener. Общий `wait()` **не читает `SO_ERROR` клиентского сокета**:
такой запрос может поглотить pending `ECONNRESET` перед обычным `readSome()`.
`Ready::native_error` передаёт известный код readiness, например `EBADF` для
`POLLNVAL`; значение 0 не заменяется случайным прежним errno.

## Проверка

| Проверка | Результат | Граница доказательства |
|---|---|---|
| Windows MSVC, полная чистая native-сборка | 54/54 PASS; 3 существующих allocation-count теста disabled | Host runtime |
| Linux x86-64, Zig 0.14.1 + WSL, полный native suite | 57/57 PASS | Host runtime |
| Scripted recovery | 299 проверок PASS | Wait/accept/readiness, transient errors, повторные open, HTTP после recovery, backoff, stop, ownership |
| Два terminal retained-close сценария | PASS | Повторные stop не закрывают неоднозначный descriptor повторно |
| Реальные TCP sockets, legacy и managed HTTP | PASS на Windows и Linux | Wait/listener fault injection, тот же порт, HTTP до/после, stop/start |
| Конкурентный sampler диагностики | PASS в обоих профилях | Чтение snapshot параллельно с worker и owner stop/start |
| IDF transport host fixtures | 3/3 PASS в Linux suite | Обычный/нулевой socket offset и integer timeout; это не аппаратная приёмка |
| TCP без admission scheduler | Linux build PASS | Отдельный compile/link профиль; не отдельный runtime-прогон |

Новые test cases выполняются значительно меньше двух минут. Длительная нагрузка,
измерения Flash/RAM плат и Wi-Fi в этой библиотечной проверке не выполнялись.
Socket/SIGPIPE regression test подтверждает, что `wait()` сохраняет pending
`ECONNRESET` для последующего `readSome()`. При изменении virtual interface нужна
полная пересборка потребителей, чтобы исключить старые объектные файлы.

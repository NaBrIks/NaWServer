# Общая очередь передачи TCP: HTTP, WebSocket и SSE

Состояние исходников на 2026-11-02, ветка `feature/unified-tx-scheduler`.
Архитектурное решение: [ADR](adr/ADR_20261102_unified-tcp-transmission.md).
Аппаратная приёмка всех подключённых плат ещё выполняется; наличие реализации
и host-тестов не является заявлением о завершении этой приёмки.

## Подключение и область действия

Общий TCP backend может использовать один ограниченный набор заданий передачи
и один Transmission worker, выполняющий native write всех его HTTP/WS/SSE
соединений. Native ESP-IDF HTTPD сохраняет собственную реализацию. Конкретный
transport и сеть запускает приложение.

```cpp
NaWServer::TcpOptions options;
options.port = 80;
options.transmission.enabled = true;
options.transmission.maximum_bytes = 32 * 1024;
options.transmission.maximum_blocks = 12;
options.transmission.maximum_blocks_per_connection = 2;
options.transmission.block_bytes = 4 * 1460;
options.transmission.step_bytes = 1460;
options.transmission.reserved_important_blocks = 1;
options.transmission.important_priority = 2;

NaWServer::TcpHttpBackend backend(transport, options);
```

При отсутствии настройки shared-режим выключен. Профили демо включают его явно.
`NAWSERVER_TCP_TRANSMISSION_BLOCK_BYTES` задаёт стандартную вместимость блока
при сборке: по умолчанию `4 * 1460` байт. Профиль lwIP может установить размер
через фактический `TCP_MSS`. Размер блока — верхняя граница выделения, а
`step_bytes` — максимум данных одной native записи в одном шаге. TCP вправе
принять только часть предложенного диапазона.

| Настройка | Смысл |
| --- | --- |
| `maximum_blocks` | Одновременно занятые слоты заданий, включая Filling; максимум реализации 32 |
| `maximum_bytes` | Предел памяти payload общей очереди; это отдельный учёт от HTTP/RX metadata |
| `maximum_blocks_per_connection` | Предел занятых заданий одного соединения |
| `initial_blocks` | Начальное число удерживаемых блоков; остальные выделяются и освобождаются по необходимости |
| `reserved_important_blocks` | Слоты, которые обычная работа не может занять |
| `reserved_important_bytes` | Защищённая ёмкость; ноль выводит её из числа резервных блоков и размера блока |
| `important_priority` | Приоритет, которому доступен защищённый резерв |
| `stall_timeout_ms` | Допустимое отсутствие прогресса у текущего головного задания соединения |
| `close_timeout_ms` | Ограничение ожидания очереди перед native close |
| `static_payload`, `static_payload_bytes` | Память payload, предоставленная приложением на всё время runtime |
| `allocator`, `min_free_bytes` | Аллокатор, размещение и нижний порог свободной памяти |

Если TX-аллокатор не задан, используются аллокатор и порог свободной памяти
`TcpOptions::buffer_memory`. Сохраняются выбранные приложением требования
internal/PSRAM/custom placement; очередь не заменяет их неявным malloc.
При наличии применяется guarded allocation. Динамическая память проверяется
на требуемое выравнивание `max_align_t`.

Поддерживаются предоставленная статическая память, начальный резерв с ограниченным
ростом и полностью динамический payload с пределами. Фиксированное выделение при
старте через аллокатор отличается от статического массива. Утверждение о static
payload не означает, что весь сервер, worker и SDK перестают пользоваться heap.

## Отдача файла или другого источника известной длины

Сервер не монтирует файловую систему и не знает LittleFS. Приложение передаёт
размер и функцию чтения, заполняющую предоставленную память:

```cpp
struct FileReader
{
    FileHandle file;

    static NaWServer::BodyReadResult read(void *context, std::size_t offset,
        std::uint8_t *destination, std::size_t capacity)
    {
        auto &reader = *static_cast<FileReader *>(context);
        const auto count = readFileAt(reader.file, offset, destination, capacity);
        return count > 0
            ? NaWServer::BodyReadResult{NaWServer::BodyReadStatus::Data, count}
            : NaWServer::BodyReadResult{NaWServer::BodyReadStatus::End, 0};
    }
};

// FileHandle/readFileAt — функции приложения, не API NaWServer.
FileReader reader{file};
return response.sendSource(200, "application/octet-stream",
    {&reader, file_size, FileReader::read});
```

`reader`, открытый файл и контекст заимствуются до возврата `sendSource`.
Отложенного хранения указателя на локальный reader нет. Файл закрывает приложение.
Функция чтения возвращает Data с 1..capacity байтами; короткие успешные чтения
допустимы. Преждевременный End, ошибка или некорректный размер обрывают ответ.
После начатого ответа второй ответ 500/503 не дописывается.

В shared-режиме чтение идёт непосредственно в выданный блок очереди. Следующие
порции запрашиваются по мере освобождения места. Расход payload RAM ограничен
очередью, а не размером файла. Для legacy transport используется небольшой
промежуточный буфер. HEAD передаёт заголовки без вызова reader.

Длина известна заранее, используется Content-Length. Этот API не означает
реализацию неизвестной длины с Transfer-Encoding: chunked. Текущий streaming wire
путь выдаёт Connection: close; managed backend после него не сохраняет сокет в
idle keep-alive. Обычные managed ответы сохраняют прежний контракт keep-alive.

## Последовательная генерация ответа

```cpp
auto result = response.beginStream(200, "application/octet-stream", total_size);
if(result != NaWServer::SendResult::Ok) return result;

for(const auto &part : parts)
{
    result = response.write({part.data(), part.size()});
    if(result != NaWServer::SendResult::Ok) return result;
}
return response.finish();
```

Каждый write потребляет переданный заимствованный диапазон до возврата.
Большой диапазон делится на ограниченные задания. При нехватке места операция
ждёт освобождения в пределах exchange/stall timeout и учитывает остановку.
Сетевой worker продолжает обслуживать другие соединения. Callback, вызвавший
write, при этом остаётся занят; другие последовательные прикладные callbacks ждут.

Суммарная длина должна точно соответствовать beginStream. Превышение длины,
finish с недостающими байтами или активным незавершённым lease — ошибка.
Функции streaming/acquire требуют соответствующей возможности backend;
неподдерживаемый backend не становится потоковым автоматически.

## Заполнение блока без промежуточной копии

```cpp
auto result = response.beginStream(200, "application/octet-stream", total_size);
if(result != NaWServer::SendResult::Ok) return result;

std::size_t produced = 0;
while(produced < total_size)
{
    auto buffer = response.acquire(total_size - produced);
    if(!buffer) return NaWServer::SendResult::TransportError;
    const auto count = generateInto(buffer.data(), buffer.capacity());
    if(count == 0 || count > buffer.capacity() || !buffer.commit(count))
        return NaWServer::SendResult::TransportError;
    produced += count;
}
return response.finish();
```

`generateInto` — функция приложения. Вместимость lease может оказаться меньше
запрошенной: учитывать нужно возвращённую capacity. acquire у Response ждёт
ограниченно; низкоуровневый Transport::acquireWrite неблокирующий.

У Response одновременно может быть один producer lease. Пока он заполняется,
очередь учитывает занятую память, но отправитель её не читает. Другие соединения
продолжают работу. commit публикует фактическую длину, reset/деструктор отменяет
неопубликованный блок. После commit/reset указатель использовать нельзя.
Lease необходимо вернуть до finish и уничтожения Response.

HTTP-блок является частью одного ответа: `discard_expired` и `replacement_key`
при его commit отклоняются. Выбрасывание середины тела испортило бы Content-Length
и границы ответа. Для HTTP сохраняется явная ошибка/закрытие.

## Явная политика WebSocket

```cpp
NaWServer::WritePolicy telemetry;
telemetry.priority = 1;
telemetry.expires_at_ms = monotonic_now_ms + 500;
telemetry.discard_expired = true;
telemetry.replacement_key = 42;
const auto result = client->send_binary(bytes, size, telemetry);
```

`monotonic_now_ms` должен использовать ту же монотонную шкалу миллисекунд, что и
backend: на host GetTickCount64/steady clock реализации worker, на ESP esp_timer,
на RP time_us_64. Нулевой срок у WS заменяется стандартным websocket send timeout.
Overload, Busy и GlobalFull обрабатывает приложение по своему сценарию.

Ненулевой replacement_key разрешает замену только не начатого matching-задания
этого соединения и только после успешной публикации нового. Filling, выполняемая
native запись и уже частично переданный кадр не отбираются. Обычная отправка не
включает удаление/слияние автоматически. Ответ команды обычно использует политику
без discard/replacement. Backend, не поддерживающий политику, отклоняет её, а не
игнорирует. Аналогичная перегрузка send_binary есть у WebSocketEndpoint.

Shared WS кодирует целый кадр непосредственно в общий блок, включая control/close.
Размер максимального кадра endpoint должен помещаться в блок с запасом заголовка;
неподходящая конфигурация отклоняется при старте. Порядок байтов одного соединения
сохраняется. Задание другого соединения может обслуживаться между native записями.

## Наблюдение за очередью

```cpp
const auto snapshot = backend.transmissionStats();
```

Снимок согласован под блокировкой очереди. Его можно получать во время работы
живого runtime; start/stop/destruction остаются сериализованными владельцем.
После отсутствия runtime возвращается пустой снимок с enabled=false. Это текущие
счётчики данного runtime, а не глобальная статистика SDK или история прошлых запусков.

| Поля | Значение |
| --- | --- |
| enabled, maximum_jobs/bytes, block_bytes, step_bytes, maximum_jobs_per_connection, reserved_important_jobs/bytes | Фактическая конфигурация общей очереди |
| stall_timeout_ms, close_timeout_ms | Настроенные пределы отсутствия прогресса и ожидания при закрытии |
| filling_jobs | Память выдана производителю, публикации ещё нет |
| queued_jobs | Опубликованные задания, включая выполняемую native запись |
| active_jobs | Native write сейчас выполняется вне блокировки таблицы |
| allocated_bytes / peak_bytes | Удерживаемая память собственных payload блоков и её пик, включая незанятый начальный резерв |
| used_bytes | Ёмкость собственных блоков, занятых заданиями/Filling |
| queued_bytes | Оставшиеся байты опубликованных заданий, включая borrowed immutable |
| borrowed_bytes | Остаток immutable диапазонов; это не дополнительная heap память |
| accepted_bytes | Суммарно опубликовано в очередь |
| sent_bytes | Суммарно принято native TCP write, включая частичные записи |
| completed_jobs | Все байты задания приняты native TCP |
| discarded_jobs/bytes | Явно разрешённые expiry/replacement до начала передачи |
| failed_jobs/bytes | Ошибки/отмена, включая незавершённые остатки при закрытии |
| rejected_jobs | Отказы попыток выделить/поставить задание; повтор producer может увеличить счётчик |
| would_block_count | Native write временно не продвинул передачу |
| last_failure / last_native_error | Последняя причина Expired/Stalled/NativeWrite/Canceled и код native ошибки, когда известен |

accepted, completed и sent **не означают TCP ACK, получение или обработку данных
приложением клиента**. Подтверждения TCP и повторную передачу обслуживает стек.
Если нужно подтверждение команды приложением, оно относится к его протоколу.

Метаданные queue/соединений, стеки, allocator overhead, RX, TCP/lwIP буферы и
управление HTTP/admission не включены в allocated_bytes. Указанные пределы не
являются общим пределом всей памяти сервера/прошивки.

## Память протоколов и исполнение

Immutable HTTP данные должны оставаться неизменяемыми и живыми до успешного
завершения runtime/server.stop. Ошибка stop с удержанным runtime не завершает
этот срок владения. Заголовки потребляются/копируются отдельно; они не получают
такого заимствования. Managed sendOwned сохраняет владение и использует ограниченную
копию в TX блоки; этот путь пока не является прямой передачей MemoryBlock в очередь.

Shared WS не выделяет прежние TX payload blocks, SSE direct-режим не держит прежний
FrameSet backlog. При этом остаются необходимые RX буферы, отложенный callback
buffer и managed private identity arena; последняя сохраняет явный admission/RX
резерв, даже если в ней есть незанятая ёмкость. Устаревшие TX копии не следует
смешивать с этим резервом при анализе памяти.

Когда включены все протоколы, сервер может иметь четыре worker:

| Worker | Задача | ESP-IDF стек/приоритет | RP стек/приоритет |
| --- | --- | --- | --- |
| HTTP | HTTP и последовательные HTTP/WS callbacks | CONFIG_NAWSERVER_TCP_HTTP_STACK_SIZE / 5 | 3072 words = 12288 байт / 3 |
| WS | Чтение, декодирование, control/close | 4096 байт / 5 | 1536 words = 6144 байта / 3 |
| SSE | Heartbeat, lifecycle и продвижение SSE metadata | 4096 байт / 4 | 1536 words = 6144 байта / 3 |
| Transmission | Единственный native writer общей очереди | 4096 байт / 5 | 1024 words = 4096 байт / 3 |

Это не включает задачи приложения и SDK. Windows/Linux используют собственные
правила резервирования стека потоков. RP поддерживает текущую CPU0-политику для
SDK/libc/server задач; произвольное SMP libc этим изменением не объявляется.

### Ожидание без холостого опроса

Пустая общая очередь не опрашивается каждую миллисекунду. Transmission worker
ждёт внутренний сигнал; commit, публикация immutable диапазона и остановка его
будят. Сигнал запоминается, если producer уведомил раньше начала ожидания;
несколько уведомлений могут объединяться, поскольку работу определяет очередь.
Отмена Filling lease тоже будит worker: она могла снять барьер порядка перед
следующим заданием того же соединения.

При ожидающей передаче сохраняется проверка с интервалом 1 мс. После native
WouldBlock/Interrupted повторная запись допустима по прежнему retry_at через 4 мс.
Частичные записи, абсолютный expiry и stall текущего головного задания сохраняют
свой контракт. Ожидание producer при нехватке памяти/flush этим изменением не
заменено общим сигналом: у сигнала один consumer.

SSE worker без клиентов ждёт сигнал, а у подписки без ожидающих данных — ближайший
heartbeat. Activate, publish, close, внешний abort и stop будят его. Пока ожидаются
передача заголовка/данных или завершение close, остаётся интервал 1 мс. Перед
reserve/activate/publish берётся актуальное монотонное время, чтобы долгий сон
не превращал новое событие в уже просроченное. HTTP/WS native wait остаётся прежним.

Уведомление не добавляет искусственной задержки до следующего idle poll; фактический
отклик зависит от планировщика, FreeRTOS tick, занятости CPU и сетевого стека.
Это не обещание жёсткого real-time. Снижение CPU на платах требует отдельного
сравнения одинаковых сценариев до/после; само наличие сигнала его не доказывает.

Каждый из двух сигналов содержит 128 байт непрозрачного встроенного хранения,
плюс признак инициализации и выравнивание. На FreeRTOS используется статический
binary semaphore, на hosts — native mutex/condition; отдельного heap выделения
для сигнала нет. Он увеличивает размер содержащего runtime/adapter; этот расход
и указатели владельца abort не входят в payload-счётчики TX. SDK заголовки через
его внутреннее переносимое объявление не распространяются.

Длительная прикладная функция задерживает следующие callbacks, но не должна
останавливать подготовленную исходящую передачу другим клиентам. Входящие WS данные
буферизуются ограниченно, затем применяется TCP backpressure; ping за такими
данными того же TCP-потока тоже может ждать. Приоритет задания не отменяет порядок
TCP и не резервирует физическую полосу Wi-Fi.

## Проверки и актуальная приёмка

Host-тесты общей очереди проверяют static/dynamic allocation, резерв байтов/слотов,
allocator floor, acquire/commit/отмену, immutable без payload allocation,
expiry/replacement, partial write, медленный сокет и отсутствие ложного stall у
задания, ожидающего за продвигающимся предыдущим. Response тесты проверяют 5/160 КиБ,
source/writer/acquire, managed/обычный путь, HEAD и обрыв источника. Mixed тест
совмещает 160 КиБ HTTP, два WS-клиента и SSE с проверкой порядка и lifecycle.
Тест сигнала проверяет notify до wait, объединение уведомлений, timeout и
пробуждение бесконечного ожидания при остановке. SSE state machine проверяет
срок heartbeat и сохранение короткого интервала при незавершённой передаче.

Сборки и аппаратные результаты нужно читать в актуальном отчёте текущей задачи.
Эта инструкция описывает контракт исходников и не заменяет аппаратную проверку,
измерение задержек, длительную устойчивость и указание фактически прошитой версии.

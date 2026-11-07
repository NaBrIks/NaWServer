# Проверка отдельного SSE-модуля, 19 октября 2026

Текущий runtime уже в `src/sse` и компоненте:
[библиотечная проверка](readme_sse_library_verification_ru.md).
Старые пути и hashes ниже относятся к историческому прогону.

Это исторический первый loopback до gated bridge. Новый этап и актуальные hashes:
[SSE integration verification](readme_sse_integration_verification_ru.md).

Результат: encoder, bounded Sessions и отдельный plain HTTPD adapter реализованы в
`tools/sse`. Host-проверки и **loopback на физической ESP32-C3** пройдены. В основной
component код не включён; API request/response и интеграция auth/handoff ожидают
решения основного чата. [Контракт и выбор решений](readme_sse_design_ru.md).

## Проверки формата и модели

Один и тот же `checks.cpp`: **20 144 проверки**, 0 ошибок на Windows host и C3.
Host: Zig 0.14.1 / Clang 19.1.7, C++17, Release, предупреждения как ошибки.
Проверки используют собственный CHECK и продолжают работать с NDEBUG.

- Golden wire и независимый line parser: пустые data, trailing newline, CR/LF/CRLF,
  ведущие пробелы, Unicode/emoji/BOM, embedded NUL в data, comment heartbeat.
- Optional id отсутствует/явно пуст, имя, retry 0/UINT32_MAX, запрет line injection;
  повреждённый UTF-8, overlong, surrogate, truncated, выше U+10FFFF.
- Exact capacity 768, расширение из-за переводов строк, отказ без изменения Frame.
- Частичные записи вплоть до 768 bytes по нескольким шагам; wrong serial, double ack,
  stale generation и reuse fd, duplicate fd, максимум 2/4 и некорректные limits.
- Индивидуальный count/bytes overflow закрывает сессию; global bytes отказ никого
  не закрывает. Idle heartbeat не является timeout всей подписки. Отдельно
  проверены no-progress stall и deadline при непрерывном медленном прогрессе.
- Приход события во время передачи heartbeat не теряет новое событие.
- Shutdown/close во время выданного write не освобождает слот до ack.
  2 000 циклов reopen/close возвращают queued bytes в ноль.

Это детерминированные проверки модели, включая на C3; они не выдают simulated
partial write или simulated time за реальные сетевые измерения.
Дополнительно существующие pipeline, request_body и url_decode host CTest: 3/3 PASS.

## Сборка, flash и устройство

Собран только C3: espressif32@6.10.0, IDF 5.4.0, GCC RISC-V
14.2.0+20241119, C++17 (`__cplusplus=201703`), -Os, CPU 160 MHz, FreeRTOS tick 1 ms.
Прошивка автономна: нет Wi-Fi, NVS init, credentials или production страниц.
Сервер и клиенты используют loopback lwIP внутри **реального C3**, не host-симулятор.

Decoder-чат `01a15228-2647-7a10-afbb-3e930d73c424` перед аппаратным этапом проверен
через wait_threads: idle/completed. Активные COM-процессы не прерывались. Текущая плата
переопределена через USB inventory и esptool chip-id: ESP32-C3 rev0.4, 4 MiB flash.
Конкретные COM/MAC находятся только в ignored local/.

Последняя прошивка: **390 208 bytes**, SHA-256
`56863040119b8539914635790748ea872ed289836ed43c2c634c948aae0e30e7`.
Записан только app0: `[0x10000, 0x6f440)`, erase sectors `[0x10000, 0x70000)`;
в пределах app0 `[0x10000, 0x290000)`. Esptool подтвердил hash записанных данных.
Bootloader, partitions, NVS, otadata, eeprom и littlefs не записывались.
Flash backup не создавался согласно актуальному owner clarification.

## Реальные HTTP/SSE/WS проверки

Последний прогон: **12 916 device CHECK**, 0 ошибок; **111 async admission и 111
complete**, active=0 и queued bytes=0 перед httpd_stop, повторный stop безопасен.
Число device CHECK включает чтение wire по байтам и может зависеть от пакетизации;
это не число независимых сценариев. Проверены:

1. Четыре одновременно открытых SSE-подписки; пятая получает 503. На четырёх
   получен ожидаемый wire с UTF-8, multiline/trailing data, именем, id и retry.
2. Last-Event-ID скопирован до ответа; непустой, отсутствующий и явно пустой headers
   различаются на настоящем HTTPD. Продолжение истории по нему не реализуется.
3. HTTP health и native WS handshake/masked echo работают рядом с четырьмя SSE.
4. Подписки пережили 1.8 s простоя с heartbeat, хотя test event deadline равен 1.5 s.
   Это проверяет отсутствие ошибочного ограничения возраста всей подписки.
5. Пять серий по 20 reconnect/close, в том числе с только что поставленным в очередь
   событием. Состояние и native async request освобождены по счётчикам.
6. Один клиент после headers полностью перестал читать; рядом две нормальные SSE,
   HTTP health и WS echo. Producer ставил следующий большой Frame только после
   ухода предыдущего из очереди: индивидуальное переполнение не подменяет stall.
   Сессия закрылась именно по Stall; Overflow-счётчик не вырос. Две другие сессии
   остались открыты и получили правильные события.
7. Stop с четырьмя активными запросами и pending data завершил requests до stop HTTPD.
   В adapter нет queued HTTPD jobs, переживающих этот drain.

В test limits: heartbeat 200 ms, stall 500 ms, event deadline 1500 ms, 256 bytes/step,
одна task и пауза 1 tick после прохода. Это ускоренные параметры эксперимента,
не рекомендация поменять документированные defaults библиотеки на эти значения.

У остановившегося читателя перед закрытием приняты в очередь 14 Frame; от начала
опыта до закрытия прошло **1478 ms**. Это включает заполнение TCP buffers и начальные
успешные передачи, а не означает превышения 500 ms no-progress timer.
Наибольший измеренный вызов adapter.publish в этом коротком прогоне — **67 μs**;
измерение включает возможное планирование. Это наблюдение, не WCET или гарантия
latency при любой нагрузке. Отправитель вызывает try-lock и никогда не ждёт сеть.

## Память и цена

На C3 `sizeof(Sessions)=12 728`, `sizeof(IdfAdapter)=12 800 bytes`; объект включает
статические очереди на максимальные 4 клиента. Установка Limits.clients=2 не
уменьшает sizeof. Одна дополнительно выделяемая task stack — 4096 bytes, плюс TCB,
два semaphore и SDK async-request/socket/lwIP allocations. Полного нулевого heap нет.
Payload budget 8192 bytes — лишь предел полезных queued bytes, не весь RAM cost.

| Число reconnect/close | Free heap после серии, bytes |
|---:|---:|
| До серий | 253 676 |
| 20 | 251 484 |
| 40 | 251 484 |
| 60 | 251 488 |
| 80 | 251 468 |
| 100 | 251 480 |

После первой серии остаётся около 2.2 KiB разницы; на последующих сериях накопления
не наблюдалось (разброс 20 bytes). TCP/lwIP может сохранять состояния закрытых
соединений; **причина разницы отдельно не локализована**. Эти замеры не доказывают
отсутствие всех утечек и не заменяют длительный soak. После остановки HTTPD/worker
heap 268 084 bytes, но это уже другая конфигурация ресурсов, не сравнительный baseline.

Размер всей test firmware по линкеру: static RAM **34 196**, flash **374 642 bytes**.
В него входят test fixtures, native HTTP/WS/lwIP и таблицы проверок; это не добавка
одного SSE-модуля в production прошивку. Собственные stack frames из -fstack-usage:
IdfAdapter::run 384, Sessions::next 368, encoder render 256 bytes; это не полные
call-chain peaks. HTTPD test stack 6144, app_main test stack 32768 bytes.

## Воспроизведение и ограничения

Команды находятся в `tools/sse/README.md`, Build.ps1, Run-C3.ps1. Чисто числовая
выжимка сохранена в `tools/sse/results/c3-summary.txt`. Сырые serial, chip identity,
build/flash logs и binaries — в ignored local/ и .pio/, не для коммита.
Первый аппаратный запуск остановился на некорректном ожидании полного WS recv одним
вызовом в test client; чтение исправлено на цикл. Добавлена default event loop для
SDK events. Затем короткая и расширенная mixed/churn проверки прошли. Неуспешный
первый запуск не засчитан как PASS; его сырой лог сохранён отдельно в local/.

Остаются: согласование production request/response handoff и общих headers,
auth/policy **до первого SSE byte через существующий pipeline**, проверка ошибок
handoff до/после bytes, Wi-Fi/browser EventSource, TLS и длительная смешанная нагрузка.
Не проверялись принудительный внешний close активного async request и stop HTTPD
в обход порядка drain — это запрещённое использование данного adapter.
Async begin при реальном исчерпании SDK heap не fault-injectировался. Модель stale
jobs/fd reuse протестирована, но это не стресс SDK с произвольными сторонними hooks.

В конце этого SSE-прогона C3 была оставлена с самостоятельной self-test прошивкой: после PASS сервер
остановлен, приложение idle; reset повторяет тест. Другие SoC не собирались.
Core/существующие примеры/LICENSE не изменены,
чужие dirty files сохранены, staging/commit/push не выполнялись. Существующая запись
проекта в локальный монитор стенда обновлена; нового выдуманного endpoint нет.

Последующая координация: основной HTTP-чат занял C3 для новой Request/Response
проверки. Указанное выше состояние прошивки историческое; SSE-чат повторно не
прошивал и не открывал COM. [Готовность и граница интеграции](readme_sse_integration_ru.md)
проверены отдельно на host; это не новая аппаратная приёмка.

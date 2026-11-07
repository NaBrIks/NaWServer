# План реализации и полной проверки TCP HTTP engine

Дата: 2026-10-24. Обновлено: 2026-10-25. Статус: P0–P5 выполнены, P6 в работе; результаты текущей реализации
ведутся в [отчёте](../readme_tcp_engine_verification_ru.md). Владелец отдельно подтвердил необходимость нагрузочных тестов с учётом
ресурсов МК. Этот план не является отчётом об уже выполненной приёмке.

Работа была остановлена владельцем после P2 на24 октября и возобновлена25 октября.
[Прежняя точка продолжения](PLAN_20261024_nawserver_tcp-engine-resume.md) сохранена;
P5 завершён, текущий checkpoint — ignored `local/tcp-engine-p5/` и поэтапный отчёт.

Читать последовательно:

1. [Решение владельца](../adr/ADR_20261024_nawserver_shared-tcp-http-engine.md).
2. [Исследование с примерами из текущего кода и источниками](../reviews/2026-10-24_shared-tcp-http-engine.md).
3. [Контракты и начальные значения](PLAN_20261024_nawserver_tcp-engine-contracts.md).
4. Настоящий план: порядок изменений, тестовые входы, критерии выхода, стенды.

## 1. Правила выполнения

- Реализовывать по одному этапу. Перед следующим сохранить diff, команды,
  результат и оставшиеся ограничения текущего. Commit/push требуют отдельного
  поручения; чекпоинт здесь означает воспроизводимый снимок/отчёт, а не auto-commit.
- До редактирования сверять current HEAD, owner changes и парные architecture/style
  документы. Изменения frontend подчиняются frontend/AGENTS.md.
- Не переписывать middleware, URL decoder, CBOR или продуктовые приложения ради
  transport extraction. Новый backend обязан проходить прежние semantic fixtures.
- Host SDK stubs — model tests. Только реальные SDK builds подтверждают сборку,
  только реальная C3 подтверждает hardware. Для IDF 5 и 6 вести отдельные результаты.
- МК в этом плане — ESP32-C3. Другие ESP32 остаются архитектурной целью без
  расходования времени на матрицу SoC. SDK builds запускать в фоне.
- Новые тестовые инструменты — C++/Go/существующий JS browser harness. Python glue
  не добавлять. Не запускать fail/load tests на чужих адресах или службах.
- Сохранить прежний HTTPD вариант до завершения сравнительной приёмки. Никаких
  автоматических fallback с TCP на HTTPD при ошибке startup.

## 2. Этапы и критерии выхода

### P0. Воспроизводимая исходная точка

Файлы: существующие tests/host sources и отчёт в docs; product code пока не менять.

- [x] Записать revision, dirty diff fingerprint, compiler/CMake/SDK versions,
  flags, test profiles, размеры бинарников и перечень активных demo processes.
- [x] Разобрать `windows_sse_test.cpp:282`: доказать разницу EOF и local completed,
  добавить ожидание наблюдаемого завершения с deadline только если это следует
  из контракта. Проверить, что genuine leak всё ещё приводит к test failure.
- [x] Повторить SSE lifecycle в 100 циклах; затем весь host suite. Не заменять
  нестабильный assert sleep или бесконечным retry.
- [x] Получить полную host-сборку; если demo exe занят, согласовать его жизненный
  цикл с текущим владельцем либо собрать новый профиль в отдельном ignored build
  directory. Не завершать чужой demo процесс только ради unlink.
- [x] Снять baseline HTTPD/C3 IDF5 по одинаковому приложению без больших встроенных
  unit fixtures, чтобы последующее сравнение размера имело смысл.

Выход: воспроизводимый baseline, известные failures названы, размеры отделены
от исторических отчётов. Текущая исследовательская проверка дала 17/18 + одиночный
SSE repeat PASS; это не выполненный P0.

### P1. TCP boundary и scripted transport

Файлы: новые tcp_transport/platform sources, deterministic tests.

- [x] Зафиксировать результат каждой операции и error mapping из контракта.
- [x] Написать scripted transport: sequence bytes, max read/write size, WouldBlock,
  Interrupted, EOF, Fatal, ручной clock, close/ownership counters.
- [x] Winsock adapter; loopback tests без HTTP (echo binary, partial I/O,
  disconnect, bind failure, accept nonblocking, restart).
- [x] Проверить listener/connection ownership и поколения slots; queued old token
  после disconnect/reuse не получает доступ к новому connection.

Выход: TCP contract test corpus проходит на scripted и Winsock. В тестах можно
принудительно вызвать short write — текущий реальный loopback сам этого не доказывает.

### P2. Общий HTTP parser/writer и Windows composition

Файлы: src/http/wire, src/backends/tcp, platform Windows glue, host CMake/tests.

- [x] Перенести bounded HTTP subset из windows.cpp в независимый byte parser.
- [x] Header views/index и один HTTP workspace; response writer из spans.
- [x] Сохранить текущую dispatcher семантику; на этом шаге owned RequestBody
  fallback ещё допустим и измеряется как известный расход.
- [x] Подключить новый backend к тем же routes/mini UI; фиксировать expected
  differences только по явно выбранной строгой parsing policy.
- [x] Добавить compile-test общих headers без Windows/IDF headers.

Выход: все HTTP wire и существующие API tests, реальный browser fetch, одинаковые
ответы application corpus; allocation ledger parser/writer готов. Новый общий
движок не содержит OS calls или version macros.

### P3. WS wire engine и SSE TCP binding

Файлы: src/websocket/wire, TCP sessions, TCP SSE Stream/worker glue; общий SSE
encoder/Sessions остаётся единственным. Windows native crypto/UTF-8 зависимости
заменяются описанными private portable helpers.

- [x] Перенести handshake/frame parser, включая fragmentation/control/UTF-8.
- [x] Убрать copies handshake metadata на каждый callback.
- [x] Ввести prepare/commit/rollback admission до 101 для нового TCP backend.
- [x] Подключить SSE reserve/activate/rollback/finish и отдельный owner worker.
- [x] Проверить HTTP+WS+SSE одновременно, forced short writes, close during send,
  server.end с активным SSE и успешный повтор после drain.

Выход: protocol suites + browser WS/EventSource + отсутствие двойного close,
поздних callbacks, перепутанных sockets. Authenticated WS не заявляется.

### P4. Предсказуемая память и нагрузочные лимиты

- [x] Ввести bounded WS TX pool; считать queued+inflight. Одна producer copy,
  раздельные frame header/payload spans; никаких временных full frame vectors.
- [x] Реализовать Individual QueueFull/GlobalFull/Busy с разными последствиями.
  Обновить callers/tests для новых enum values. Отдельный regression для
  close-on-individual-overflow согласно принятому local-device ADR.
- [x] Подготовленное TCP body → borrowed Request view без второй полной копии;
  прежний owned fallback сохранён для backend, который читает через readBody.
- [x] Не держать тяжёлый receive/workspace для каждого TCP slot без необходимости.
  Admission allocations bounded, проверены failure injection и rollback.
- [x] Замерить sizeof/storage reservations и число/bytes allocations/copies
  на GET, POST4096, WS4096, fragmented WS и SSE publish/send.

Результаты P4: host42/42, browser18/18+repeat, runtime allocation-failure ledger,
14 C3 common objects+probe и native HTTPD SDK regression PASS. Allocation counters
измерены; copy accounting основан на pointer identity/wire equality и source,
не на полном memcpy tracer. См. точные границы в поэтапном отчёте.

Выход: allocation ledger объясняет все оставшиеся расходы, wire hot path не
аллоцирует на fragment/header/send step. Query/form maps, convenience APIs,
SDK allocations и внешние endpoint allocations посчитаны отдельно. Отчёт не
называет всю библиотеку zero-heap.

### P5. Linux adapter и изолированная приёмка

- [x] Linux sockets adapter с poll/MSG_NOSIGNAL; CMake GCC и Clang, C++17.
- [x] Выделить переносимый host demo entrypoint и явные bind address/port options,
  используя root frontend assets. Defaults — loopback и непривилегированный port.
- [x] Запустить тот же byte/application/browser corpus; sanitizer и fuzz этапы
  из разделов ниже; проверить high fd, EINTR, reset и restart.
- [x] Проверить SIGPIPE/disconnect без изменения signal policy процесса.
- [x] Получить доказательство bounded CPU/memory/threads/file descriptors/logs.

P5 evidence: Windows44/44; Linux GCC41/41, Clang41/41, ASan/UBSan41/41;
TSan runtime и отдельный pool concurrency oracle. Два hour jobs завершились:
HTTP11430890/3602s, WS127473900/3601s, sanitizer findings0. Реальные browser,
high-fd/EINTR/SIGPIPE/restart и bounded resource sample проверены. Это не P7 load/soak.

Выход: native Linux binary действительно запущен. Кросс-компиляция Windows→Linux
сама по себе не является runtime PASS. Предпочесть локальную Linux VM/имеющийся
WSL2 или отдельный стенд; их наличие предварительно проверить, не предполагать.

### P6. ESP-IDF TCP adapter, два SDK, два backend

Историческая read-only подготовка, SDK inventory/source findings и исходная bounded
lwIP close policy: [P6 handoff](PLAN_20261025_nawserver_idf-tcp-adapter.md).

- [x] Подключить BSD sockets/worker glue IDF без HTTPD dependencies.
- [x] Разделить source/dependency lists и build-time backend choice. Проверить
  HTTP-only, WS-only относительно SSE, SSE-enabled и WS+SSE profiles.
- [x] Native HTTPD собрать под 5.4.0 и 6.0.3; различия только в SDK adapter.
- [x] TCP-only собрать под обоими SDK на C3. Если PlatformIO pin не предоставляет
  IDF6, использовать отдельный официальный IDF environment и build directory,
  не выдавать заменённый framework внутри старого pin за прежний toolchain.
- [x] Проверить map/dependency graph: HTTPD отсутствует в TCP-only standalone
  minimal; TCP wire engine отсутствует в HTTPD-only minimal.
- [ ] На повторно идентифицированной C3 проверить оба backend на обоих SDK.
  Писать только проверенные intended app ranges; без backups и дополнительных
  post-flash hash/readback passes. Не менять Wi-Fi secrets/partition/NVS ради теста.

Все четыре SDK/backend строки реально прошиты и прошли on-device HTTP/loopback/
lifecycle, LAN HTTP/WS и отдельные bounded SSE admission/drain/stop/restart probes.
Последний пункт пока открыт: полный последовательный browser corpus воспроизводит
необъяснённые WS/SSE failures, в том числе на native baseline. Точная текущая
граница и diagnostic-only images записаны в verification report; сборки16/16 и
один успешный browser repeat не заменяют завершённую приёмку P6.

Выход: четыре реальные строки C3 runtime matrix, единое приложение и независимые
ресурсные результаты. Сборка SDK stub не закрывает ни одну из этих строк.

Приоритет владельца уточнён25 октября: сначала рабочий MVP C3, host-проверки
быстрые и поверхностные. Текущий safer-close core реально проверен на C3 под
5.4.0/6.0.3: on-device89/63/30, LAN HTTP/WS, SSE admission3/stop2 и обычный браузерный
режим (постоянный WS+2SSE+HTTP,20 iterations, exact delivery/drain) PASS. На плате
оставлен обычный TCP image6.0.3. Это функциональная точка MVP, не полная приёмка
P6: быстрые переподключения Chrome всё ещё воспроизводят timeout; расширенная
матрица, нагрузка и длительная проверка отложены. Подробности —в текущем отчёте.

### P7. Приёмка нагрузки, длительной работы и совместного Linux-стенда

- [ ] Выполнить функциональную/нагрузочную матрицу ниже на Windows, isolated Linux,
  C3 IDF5/6 TCP и baseline HTTPD там, где capability совпадает.
- [ ] Проверить длительную работу и восстановление после отказов. Прекратить
  прогоны с явным panic/leak/heap exhaustion; сначала устранить причину.
- [ ] Только после isolated Linux gate — bounded process рядом с ioBroker
  по runbook раздела 6. Проверить ioBroker health до/во время/после.
- [ ] Обновить readme guides, capabilities, architecture EN/RU и отчёт. Удаление
  временной дублирующей Windows реализации допустимо после parity; HTTPD остаётся.

Выход: отсутствуют unexplained failures, результаты и ограничения воспроизводимы,
память/задержки не заменены общим «всё PASS».

## 3. Общий формат свидетельств

Для каждого прогона сохранять в ignored local/: command, time, source fingerprint,
platform/architecture, compiler/SDK revision, sdkconfig/profile hash, backend,
ports/target identity, seed, duration, inputs, counters и exit code. В отслеживаемом
отчёте — обезличенные итоги/числа и объяснение. Credentials/raw serial/firmware
остаются ignored. Не помещать данные других сервисов в test artifacts.

Отдельные графы: build, deterministic unit, wire/socket integration, browser,
hardware, stress, soak. `Skipped` и `Unsupported` не превращать в PASS.
Differential тест сравнивает status/headers/body/application effects; wire-parser
различия native HTTPD отмечаются capability/SDK-specific, не маскируются одинаковым
набором wrapper объектов. Динамические поля/порядок HTTP headers нормализовать,
payload bytes и статусы ошибок не нормализовать произвольно.

## 4. Функциональная и отказная матрица

| ID | Сценарий | Обязательное наблюдение |
|---|---|---|
| T01 | Каждое read/write ограничить 1, 2, 3, 7, 127 B | Payload точен, offsets не теряются, нет ожидания «весь пакет за recv» |
| T02 | WouldBlock/Interrupted до/между progress | Нет busy loop, тот же absolute deadline, stop замечен |
| T03 | EOF/reset/fatal на каждой фазе, в том числе partial response | Нет второго response, use-after-free, утечки slots |
| T04 | Accept/bind/start failure; failure после каждого allocation | Один rollback/close; возможность повторного start или CleanupRequired |
| T05 | Повтор fd/slot, старый queued token, generation overflow model | Старое действие не касается нового клиента |
| T06 | Stop из callback; stop при partial read/write; повтор end | InvalidState либо корректный bounded join; callback после success отсутствует |
| H01 | Все точки разбиения request line/head/body; head+body одним recv | Те же route/response, не теряются prefetched bytes |
| H02 | Limit−1/limit/limit+1 по head/target/header count/body | Точные boundary outcomes из contract, без overflow |
| H03 | CL duplicate/list/sign/overflow, TE, CL+TE, obs-fold, NUL, bare LF | Строгий отказ, endpoint не вызван; next request не исполняется |
| H04 | HTTP1.0/1.1, Host missing/duplicate/empty, unsupported version/method | Соответствие выбранному profile и таблице статусов |
| H05 | Binary/NUL body; GET/POST/HEAD/204/304; static binary/gzip | Payload/length/MIME корректны, HEAD не имеет body |
| H06 | Query/form malformed escapes, duplicates, precedence, raw CBOR | Прежняя semantics; partial parameters не видны |
| H07 | Basic/auth/access, missing response, double send, send failure | Fail closed, один pipeline executor, один response |
| H08 | Два request в одном TCP write; тело без CL | Нет скрытого keep-alive/pipelining или угадывания body |
| H09 | TCP half-close после полного запроса; большой rejected upload | Корректный ответ где transport позволяет, bounded drain и release |
| W01 | Known handshake vector, malformed/duplicate fields, bad key/version | Нет 101 до validation/reservation; onConnect ровно один раз |
| W02 | 101 split/failed send, pool exhaustion до upgrade | Rollback; onConnect не вызывается при неполном handshake |
| W03 | Frame lengths 0,1,125,126,127,4095,4096,4097; 64-bit length headers | Проверка до allocation, no narrowing/overflow; correct close code |
| W04 | Mask offset на каждом split; несколько frames в recv | Точный message payload, нет потери хвоста |
| W05 | Fragmented text/binary и ping между fragments | Один bounded onMessage; control не разрушает assembly |
| W06 | UTF-8 split, overlong, surrogate, >U+10FFFF; bad close reason | Корректный отказ/close, а не platform-dependent Unicode repair |
| W07 | Invalid RSV/opcode/unmasked/control fragmentation/continuation | Protocol close и освобождение |
| W08 | Concurrent producers, copy source mutated сразу после Queued | В wire исходные owned bytes; никакого network wait под producer lock |
| W09 | Individual full, global full, lock busy, oversize | Различимые результаты; закрытие только по соответствующей политике |
| W10 | Partial output + ping/close + timeout/disconnect | Не перемешаны frame bytes; TX pool возвращён полностью |
| S01 | Auth до SSE open; invalid Last-Event-ID; reserve/activate failures | Нет bytes до commit, rollback возвращает original owner |
| S02 | UTF-8/multiline/empty event/id/retry/heartbeat | Совпадает с существующим encoder/browser contract |
| S03 | 4 subscribers, excess subscriber, individual/global queue pressure | Bounded resources, правильный refused/closed result |
| S04 | Stop/abort/close while write; EOF vs local completion | bounded eventual completion; begun/completed/active согласованы |
| S05 | SSE + обычный HTTP + WS, затем stop и restart | Нет двойного close, stale stream или late callback |

Для небольших fixtures перебрать каждую точку split, для больших — boundary
набор плюс воспроизводимые random splits. Fuzz parser/WS decoder отдельно с
ASan/UBSan: начальный gate ≥1 час на parser и ≥1 час на WS codec, фиксировать
coverage/corpus/seed и сохранять каждый crash как regression. Это не доказательство
отсутствия всех багов. TSan запускать отдельно на очередях/lifecycle, если platform
toolchain поддерживает; не смешивать с измерением производительности или RAM МК.

При adversarial wire тестах не сравнивать native HTTPD с собственным parser как
с абсолютным oracle: сравнивать каждый с его документированными обязательствами.
Прикладные ответы и доступ проверяются одинаково. Для crypto/vector tests использовать
независимый oracle, а не копию тестируемого алгоритма.

## 5. Нагрузка для микроконтроллера

### 5.1 Цель и профили

Цель — устойчивый локальный контроллер при 2–4 клиентах, предсказуемый отказ
за лимитом и освобождение ресурсов. Не целиться в desktop RPS и не ставить
unbounded flood на C3. Генератор нагрузки работает на ПК, а не на МК.

Четыре клиента не равны четырём sockets. Начальный max_connections=7 не позволяет
одновременно 4 WS + 4 SSE + HTTP. Нужны явно разные сценарии:

| Профиль | Соединения/действия | Длительность первого gate |
|---|---|---|
| L0 idle | Server started, 0 clients; затем после полного drain | 10 min |
| L1 normal-2 | 2 browser-like клиента, каждый HTTP reload + WS; SSE у одного | 15 min |
| L2 normal-4 | 4 WS + до 2 параллельных HTTP; SSE отдельным следующим прогоном | 30 min |
| L3 mixed | 2 WS + 2 SSE + до 3 HTTP; общий предел 7 | 30 min |
| L4 SSE-4 | 4 SSE + до 2 HTTP, 5-й SSE должен получить отказ | 15 min |
| L5 full-4 optional | 4 WS + 4 SSE + 2 HTTP, explicit max_connections≥10 и согласованный SDK socket budget | Только после проверки RAM/sockets; 30 min |
| L6 overload | По ступеням активный предел, предел+1, предел+4; короткие bounded попытки | 60 s на ступень, 5 циклов |
| L7 churn | Connect/request/close, затем WS/SSE reconnect; 1000 циклов | Rate сначала 2/s, максимум 10/s после стабильного прогона |
| L8 lifecycle | start/open/send/drain/stop; 100 циклов | Проверять все результаты и возврат ресурсов |
| L9 soak | Стабильный L1/L3 с редкими reconnect и короткими overload windows | Предварительно 2 h; приёмка 24 h |

L5 не является скрытым новым default. Если C3 не допускает его по памяти, отчёт
честно указывает поддержанный concurrent profile; L1–L4 обязательны при выбранных
лимитах. Существующие SDK max-open-sockets, listener/control sockets и другие
сервисы приложения учитываются отдельно. Native HTTPD и TCP сравниваются на
одинаковой достижимой конфигурации, а не разных числаx открытых клиентов.

Нормальная нагрузка: WS commands/echo 128 B, 10 messages/s/client; SSE 256 B,
5 events/s/subscriber; HTTP /health раз в секунду и последовательная загрузка
реального shared UI. Отдельные ступени payload 1 KiB/4 KiB с тем же и уменьшенным
rate показывают пределы. Насыщение TX queue проверять специальным сценарием,
не считать преднамеренный QueueFull в overload отказом нормального профиля.

### 5.2 Медленные и неисправные клиенты

- Один читатель не читает после headers; другой читает по 1–16 B с задержкой.
  Проверять и no-progress timeout, и общий deadline при регулярном малом progress.
- Медленный sender передаёт header/body по одному byte; deadline нельзя бесконечно
  продлевать каждым байтом. После отключения normal HTTP/WS progress восстанавливается.
- Одновременно static asset transfer, WS командный echo и SSE publisher.
  Проверять очередность, результаты send и время отказа, а не только отсутствие crash.
- Переполнение индивидуальной очереди и глобального pool — отдельные тесты.
  При отказе команды результат явный, никто не выдаёт Queued за ACK.
- Disconnect/reset во время headers, body, 101, WS payload, SSE preface/event.
  Проверять close reason/counters и возвращение storage slots.
- Потеря LAN/переподключение Wi-Fi проверяется приложением отдельно от HTTP core.
  После network-ready приложение может перезапустить backend; core не управляет Wi-Fi.

Один HTTP/WS worker может задержать остальные callbacks до конца текущей
синхронной операции. Измерять это отдельно от SSE progress. Если normal-load
задержки не проходят gate, остановить расширение и пересмотреть execution design
по измерению; добавление буферов или увеличение deadline не считается исправлением.

### 5.3 Метрики и критерии

Предлагаемые начальные acceptance gates (инженерные, уточняются с объяснением
по baseline; не подгоняются задним числом ради PASS):

| Метрика | Gate / что записывать |
|---|---|
| Correctness | 0 payload corruption, wrong-route responses, double callbacks, unexpected disconnects в normal profiles |
| Latency normal L1–L3 | /health и WS command echo p50/p95/p99/max; начальная цель p95≤500 ms на локальной сети, max≤configured exchange deadline |
| Mixed-load regression | При p95 росте >25% против сопоставимого HTTPD baseline — объяснение и решение до объявления готовности |
| SSE progress | Стабильные events при normal load; нет ложного stall из-за блокировки другого owner |
| Producer latency | Нет ожидания socket; bounded copy/try-lock. Записать p95/max, отдельно Busy/GlobalFull/QueueFull |
| Timeout | Закрытие в configured bound + измеренная scheduler tolerance; deterministic tests без tolerance |
| Recovery | После снятия overload все слоты/очереди освобождены и /health отвечает в течение 2×exchange deadline |
| C3 heap | Free/minimum/largest block до start, после start, при 1/2/4 клиентах, peak load, после drain |
| C3 reserve | Начальный gate: minimum internal free ≥32 KiB, largest block ≥8 KiB, measured stack headroom ≥1 KiB; несоответствие требует пересмотра профиля |
| Leak trend | После warm-up/drain нет монотонного роста owned allocations; снижение free heap на ≥1 KiB расследуется, а не списывается на погрешность |
| Windows/Linux resources | Private bytes/RSS, threads, handles/fds, CPU idle/active, pool usage и возврат после drain |
| Failures | 0 panic/watchdog/ASan/UBSan findings, deadlock, stale-token access; transient test failures расследуются |

Публиковать bytes и KiB (1024 B), не смешивать decimal KB. Every ≥1 KiB delta
между backend/SDK сопровождается объяснением. Выигрыш Flash не оправдывает
неизвестный рост peak RAM или потерю responsiveness.

Отдельный размерный профиль без unit fixtures: HTTP-only, +WS, +SSE, +WS+SSE.
Для каждого IDF version/backend записать `.text/.rodata`, `.data/.bss`, image size,
heap, sizeof ключевых объектов, task count/stack reservation/high-water. Одинаковые
optimization/log/assert/partition/Wi-Fi settings и одинаковые frontend assets.
Санитайзерные host builds и старые прошивки с тестовыми Capture массивами не
использовать для вывода «свой HTTP экономит столько-то».

## 6. Linux на компьютере и рядом с ioBroker

### 6.1 Где проверять

На текущем Windows-компьютере уже есть host demo и tests; будущий общий движок
проверяется здесь через реальный loopback/browser. Для именно Linux-поведения
нужен Linux runtime: имеющийся WSL2/VM/отдельная машина. Наличие WSL/VM и их
состояние в исследовании не проверялись. Cross-build без запуска недостаточен.

Машина с ioBroker пригодна для отдельного bounded Linux executable после P5.
Это не adapter ioBroker, не plugin и не HTTP route его процесса. Source остаётся
в NaWServer; host folder содержит только test deployment. Рабочие файлы и
config ioBroker, Node.js и другие сервисы не меняются.

### 6.2 Runbook перед deployment

1. Read-only проверить OS/arch/libc, systemd version, user identity, свободный
   numeric high port, CPU/memory/disk headroom и текущий health ioBroker.
2. Проверить доступную изоляцию: отдельный непривилегированный user либо уже
   настроенный rootless container с лимитами. У user не должно быть права менять
   production data. Не запускать executable от root.
3. Собрать release бинарник в isolated Linux matching architecture/libc;
   подготовить version/hash/profile и bounded logs в отдельной test directory.
4. Первая привязка — loopback, отдельный port; доступ к browser через имеющийся
   SSH tunnel. Не менять firewall, reverse proxy или production ports ради smoke.
   До нагрузки сверить PID/port и ответ NaWServer с уникальным run identifier;
   один HTTP 200 не доказывает, что клиент попал в нужный процесс.
5. Запускать transient unit, без enable/autostart, с CPU/memory/pids/runtime limits.
   Проверить установленные man/help и перечитать effective values/cgroup files.
   Не считать успешный запуск доказательством применения resource controller.

Начальный функциональный профиль изоляции: CPUQuota=25% одного CPU,
MemoryHigh=96 MiB, MemoryMax=128 MiB, MemorySwapMax=0, TasksMax=16,
RuntimeMaxSec=1800, Restart=no, NoNewPrivileges=yes. Это ограничение тестового
host процесса, не модель RAM МК. Перед 24h soak RuntimeMaxSec задаётся отдельно,
лимиты памяти/CPU сохраняются. Если свойство не поддерживается или user delegation
не действует, не запускать нагрузку без фактического ограничения.

Пример формы команды для будущего runbook, **не готовая команда этой ревизии**:

```text
systemd-run --user --unit=nawserver-test-<unique-run-id>
  --property=Type=exec --property=CPUQuota=25%
  --property=MemoryHigh=96M --property=MemoryMax=128M
  --property=MemorySwapMax=0 --property=TasksMax=16
  --property=RuntimeMaxSec=1800 --property=Restart=no
  --property=NoNewPrivileges=yes
  <absolute-tested-linux-demo-path> <verified-loopback-port-arguments>
```

Здесь аргументы demo ещё не существуют; добавить/проверить их в P5. На сервере
сверить [systemd-run](https://www.freedesktop.org/software/systemd/man/latest/systemd-run.html)
и [resource-control](https://www.freedesktop.org/software/systemd/man/latest/systemd.resource-control.html)
с установленной версией. Логи ограничить отдельным размерным budget 16 MiB;
не менять глобальный journald config. Sanitizer builds с большим virtual address
space не пытаться втиснуть в этот functional memory profile.

### 6.3 Допустимая нагрузка и прекращение

Генератор работает на Windows/отдельном стенде, целится только в выделенный port.
Первый remote smoke: 1–2 клиента, затем normal L1/L3 в пределах quotas. Выполнение
полной функциональной матрицы не требует socket flood. Stress/fuzz/forced-OOM и
предельные churn тесты остаются в isolated Linux; на ioBroker допустим длительный
bounded normal-load soak, если предыдущие этапы стабильны.

Перед remote test снять 10 min baseline health/latency и ресурсов машины.
Остановить только test unit, если ioBroker теряет health, задержка его проверяемого
health endpoint растёт более чем вдвое от baseline в трёх последовательных замерах,
свободная RAM машины падает ниже 20%, появляются memory pressure/OOM события или
ограничения test cgroup не подтверждаются. Эти thresholds — начальная policy
runbook, их не считать текущими измерениями машины.

После остановки: test unit inactive, test port свободен, дочерних процессов нет,
ioBroker health соответствует baseline. Сохраняются только ограниченные логи
тестового процесса. Не делать systemctl restart ioBroker для «восстановления теста».
Если наблюдается влияние — завершить эксперимент и исследовать isolated.

## 7. Матрица завершения

| Платформа | HTTPD | Общий TCP | Unit/wire | Browser | Load | 24h soak |
|---|---|---|---|---|---|---|
| Windows x64 | — | Functional P5 |44/44 PASS|18/18+repeat PASS| Pending | Pending |
| Linux native isolated | — | Functional P5 |GCC/Clang/ASan41/41,1h wire fuzz PASS|18/18+repeat PASS| Pending | Pending |
| C3 / IDF5.4.0 |Build PASS/runtime Pending| Pending |Compiler-only; runtime Pending| Pending | Pending | Pending |
| C3 / IDF6.0.3 | Pending baseline | Pending | Pending | Pending | Pending | Pending |
| Linux на машине ioBroker | — | После isolated gate | Functional subset | Pending | Bounded normal only | После health/quota gate |

Для C3 soak HTTPD и TCP вести раздельно; разрешено последовательное выполнение
на одной идентифицированной плате. Одновременно прошивать её из двух процессов
нельзя. Отчёт для каждой строки содержит actual profile, а не только названия SDK.

Готовность: пройдены этапы P0–P7 в заявленном scope, нет unexplained failures,
backend selection проверен по link map, memory/copy ledger опубликован,
runtime differences описаны. Если часть платформ/длительных тестов не выполнена,
оставить строки Pending с точной причиной. Общая фраза «поддерживает Windows,
Linux и IDF 5/6» допустима только в пределах фактически завершённой матрицы.

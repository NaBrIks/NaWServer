# Реализация общего TCP HTTP/WS: этапы и проверки

Начало работы: 24 октября 2026. Исходный HEAD:
`63ee25c55a332ce8078df625da194c9f48cebaa1`.

Это отчёт текущей реализации по
[плану](plans/PLAN_20261024_nawserver_tcp-engine-implementation.md),
а не перенос исторических результатов в новую матрицу поддержки.
Native HTTPD сохраняется. Commit/push не выполняются.

## P0: исходная точка

До изменений сохранены чужой tracked diff и SHA-256 каждого изменённого или
неотслеживаемого файла. Исходный tracked diff fingerprint:
`135B7E14738FD0611983B93E7EF3DCC4F2CA33FBEBF8D30EC9062A953E8CFFEC`.
Он включает изменения правил, пар architecture/style, formatter и документации.
Парные EN/RU документы проверены по времени и содержанию; расхождений правил
не обнаружено. Эти изменения сохраняются.

Стенд Windows x64: Zig 0.14.1, C++17, CMake 4.0.2, Ninja; core/backend
`-Wall -Wextra -Werror -pedantic`. Новый host build расположен отдельно в
ignored `local/tcp-p0-host/`. Существующее demo PID 7976 из
`local/http-host-build/host/` не останавливалось и не перезаписывалось.
Его старые бинарники не считаются свежей сборкой.

Причина SSE assert после EOF установлена по canonical sources:
`WindowsSseStream::finish()` делает shutdown write; затем
`QueuedAdapter::poll()` отдельно подтверждает `sessions_.completed()` под lock.
Клиентский EOF может наблюдаться между этими действиями. Тест должен дождаться
локального завершения с deadline. Socket receive error не равен EOF.
Product code на P0 не меняется.

Свежие host результаты:

- Полная сборка, включая новый demo executable: PASS.
- `ctest -R '^windows_sse$' --repeat until-fail:100 --output-on-failure`:
  100/100 PASS, 20.46 s. Запуск из `local/tcp-p0-host/`.
- Затем `ctest --output-on-failure`: 18/18 PASS, 4.15 s; SSE 409 checks.
- Completion waiter требует одновременно active=0, queued_bytes=0 и ожидаемый
  completed. Fake-clock cases отвергают зависший active и потерянный completed;
  отдельный negative case удерживает настоящую открытую SSE session и проверяет
  отказ того же waiter. Произвольный sleep вместо условия не добавлен.
- Debug demo image: 1703424 B (1663.5 KiB); static archives HTTP 2833090 B,
  Windows 3449558 B, memory 1545746 B. Это host debug file sizes, не RAM/Flash МК
  и не размер нового engine.

Размерный HTTPD/C3 baseline (реальная фоновая SDK-сборка, без прошивки): PASS,
327.16 s. PlatformIO 6.2.0 / espressif32 6.10.0 / pinned ESP-IDF 5.4.0,
GCC 14.2.0; отдельное приложение
`local/tcp-engine-p0-c3/`, HTTP+WS+SSE и shared mini UI, без больших unit fixtures.
Linker Flash 888784 B (867.95 KiB), static RAM 42544 B (41.55 KiB), app image
889184 B. SSE backend object 12952 B (12.65 KiB); это reserved storage, а не
queued bytes. SDK optimization DEBUG/-Og, assertion level 2, INFO logging,
HTTPD head/URI 1024/512 B, WS включён, lwIP sockets 10, FreeRTOS 100 Hz.
Сравнивать последующие варианты только при тех же actual flags и приложении.
Runtime heap, stack high-water и hardware для этой сборки ещё не измерены.
Final incremental build PASS 8.91 s; actual SCons compiledb и static_assert
подтвердили `-std=gnu++17 -Og -fno-exceptions -fno-rtti -fstack-usage`.
SDK CMake 3.16.4 / Ninja 1.9.0 отличаются от host инструментов выше.

P0 закрыт для перехода к P1: известная гонка устранена в тесте, все host checks
прошли, SDK size baseline получен. Финальный test source SHA-256:
`21D52CD68E99C74B2BEAE02B5EED10E7C942701B78FE1CF4A4D2FA3AE79FEF3B`.

Сырые команды, исходные fingerprints, конфигурации и build evidence находятся в
ignored `local/tcp-engine-p0/`, `local/tcp-p0-host/`, `local/tcp-engine-p0-c3/`.
Credentials, device identity и raw serial в этот отчёт не включаются.

## P1: TCP boundary и владение (завершён)

Добавлен SDK-independent `backends/tcp_transport.h`: numeric IPv4 listener,
nonblocking accept/read/write, caller-owned wait interests/results, shutdown write
и close с явным результатом освобождения. Native value 0 допустим. Отдельные
результаты Progress/WouldBlock/Interrupted/PeerClosed/Fatal не смешиваются с timeout.
Полная нормализация версионных lwIP/Linux ошибок ещё не реализована.

`ConnectionSlots` использует caller-owned storage и token slot+uint64 generation.
prepareClose переводит слот в Closing, native close выполняется вне table lock,
completeClose подтверждает освобождение. Retained failure сохраняет слот;
released-with-error не повторяет native close. Generation не переполняется:
исчерпанный слот окончательно retired. Таблица сама не запускает I/O и не создаёт
mutex; будущий runtime обязан сериализовать доступ и cleanup одного I/O owner.

Свежий ownership corpus: 115 checks PASS, включая stale write/close/completion,
повтор native value, дубликат при другом свободном слоте, наблюдение Closing
внутри fake syscall, retained/released close errors и generation exhaustion.

Настоящий C3 GCC compile-only gate: C++17, warnings-as-errors,
`-fno-exceptions -fno-rtti`, include только `include/` и `src/`: PASS.
Общие HTTP/WS/SSE/TCP headers не требуют OS/SDK headers.

| Хранилище | Windows x64 | C3 ABI, compile-only |
|---|---:|---:|
| Handle | 16 B | 8 B |
| Ownership slot | 32 B | 24 B |
| Семь slots | 224 B | 168 B |
| Table facade | 16 B | 8 B |
| WindowsTransport | 24 B | — |
| Три Winsock wait sets, временный стек вызова | 1560 B (1.523 KiB) | — |

У TCP adapter нет payload storage или собственных payload allocations. Этот ledger
не включает будущие HTTP workspace, WS pools, SSE runtime, task stacks и память ОС.
Native Winsock failure-injection для WSAEINTR/rollback-close ещё не выполнялся;
соответствующие нормализованные результаты проверяются scripted fixture,
а native ветви отдельно просмотрены агентами. Это разные виды evidence.

Итоговый общий host build PASS; затем CTest **21/21 PASS, 3.96 s**:

- Scripted TCP: 5596 checks PASS, forced short read/write 1/2/3/7/127 B,
  WouldBlock/Interrupted/EOF/Fatal, binary guards, wrong-handle/wait-order oracle,
  retained setup cleanup, close counters, manual clock и bounded exhaustion.
- Winsock: 3331 checks PASS в этом прогоне; binary 4096 B, ответ после peer
  shutdown write, reset, exclusive bind failure, nonblocking accept/read,
  duplicate wait entries, independent owners и 100 restart cycles. Число checks
  зависит от фактического числа wait/partial steps, проверяемые сценарии фиксированы.
- Slots: 115 checks PASS; common-header compile PASS на host и C3 GCC.
- Uncrustify только новых/изменённых файлов и git diff --check: PASS.
- Два независимых агента просмотрели ownership/adapter/fixture; найденные пробелы
  wrong-handle oracle, retained setup failure и промежуточного Closing добавлены
  в corpus до gate. Блокирующих findings не осталось.

Fixture storage 34104 B (33.3 KiB) — только host-тест, не production storage.
Он ограничен 128 steps, 8 handles, 8 interests, 16384 B capture; borrowed input
должен жить до потребления шага. Реальный forced short syscall в Winsock этим
прогоном не доказывается; соответствующее evidence — scripted.

Evidence: `local/tcp-p1-build.log`, `local/tcp-p1-suite.log`,
`local/tcp-p0-host/Testing/Temporary/LastTest.log`, `local/tcp-p1-c3/`,
`local/tcp-p1-windows/`, `local/tcp-p1-scripted/`.
P1 закрыт для перехода к P2. Это ещё не HTTP/WS engine, Linux adapter или SDK TCP build.

## P2: общий HTTP wire path и Windows composition

Реализованы `http::wire::RequestParser` и `ResponseWriter`, синхронный SessionIo,
общий TCP HttpExchange и `TcpHttpBackend`. Parser/writer не вызывают OS API;
Windows worker и Winsock находятся в отдельных platform sources. Public headers
не требуют SDK. Constructors инертны; concrete transport.start и server.begin
вызываются явно. Native HTTPD и прежний Windows backend сохраняются.

Parser читает неподвижное head storage напрямую, индексирует offsets/views,
не создаёт строки для каждого header/target. Framing проверяется до dispatcher;
неизвестный синтаксически допустимый method теперь Unknown/обычно404 по контракту,
вместо старого Windows501. Bare LF, obs-fold, bad Host/CL/TE и точные границы
проверяются отдельно от application semantics. Writer сначала проверяет весь
ответ, затем отдаёт короткий prefix и borrowed spans; полной копии response нет.

`executeHttpConnection` вызывает тот же HttpRequestHandler/dispatcher: body →
query/form → handlers → middleware → endpoint. Prefetched bytes не теряются;
тело ограничено объявленной длиной, второй pipelined request не исполняется.
P2 сохраняет прежний owned RequestBody fallback и его vector. Отдельного
дублирующего body buffer4096 в HTTP workspace пока нет. Миграция на borrowed
prepared body относится к P4.

SessionIo проверяет stop и абсолютный exchange deadline между bounded syscalls;
readiness, WouldBlock и Interrupted не считаются прогрессом. No-progress deadline
сбрасывается положительным I/O и сменой read/write phase, но total не продлевается.
Read EOF не запрещает ответ на уже полученный request. Финальный drain ограничен
4096 B/50 ms; небольшой overlimit upload — 250 ms; оба входят в общий budget.
Произвольный заблокированный callback транспорт не прерывает.

Stop из callback отклоняется. Stop timeout сохраняет worker, sockets, workspace и
handler для повторного end; join/delete выполняется после выхода owner. Сохранена
прежняя facade semantics: при неуспешном end running остаётся true, отдельный
cleanupRequired flag относится к failed startup. Startup/accept result теперь
передаёт optional предыдущий CloseResult: nullopt означает отсутствие close
attempt, известный retained WouldBlock допускает один retry на явный stop,
ambiguous Fatal сохраняет ресурсы без слепого повторного close.

Свежий общий host build PASS; CTest **28/28 PASS, 8.97 s**:

| Проверка | Результат |
|---|---|
| Parser, все splits/границы/ошибки | 29550 checks,5826 split runs PASS |
| Writer, exact wire/partial I/O/borrowed lifetime | 63623 checks PASS |
| SessionIo, scripted deadlines/stop/errors | 344 checks PASS |
| Реальный dispatcher поверх scripted TCP | 1107 checks,79 exchanges PASS |
| Прежний HTTP socket corpus с TCP composition | 120 checks PASS |
| Lifecycle/rollback/blocked callback | 374 checks,10 restarts PASS |
| Private Windows worker | 440 checks,100 cycles PASS |
| Прежние HTTP/WS/SSE и остальные suites | PASS в том же полном прогоне |

Контролируемый stop задан20 ms, в полном прогоне измерен42 ms. Это host scheduler
tolerance, не deterministic deadline proof; в scripted SessionIo границы точные.
Native setup/close ошибки, которые нельзя воспроизвести обычным loopback, не
выдаются за реально fault-injected Winsock branches.

Два demo используют общий `host/http_demo.h`: те же routes/endpoints/auth/chain и
root frontend assets. TCP demo — HTTP-only profile, WS/SSE ещё P3. Реальный Edge:
12/12 browser fetch PASS и повтор PASS; JavaScript errors0, отдельный CSS,
desktop/mobile/dark screenshots, page overflow отсутствует. Финальное собственное
demo стартовало на ephemeral loopback port и штатно завершилось по timer15s,
browser exit0/demo exit0. Чужое demo не останавливалось.

### Ledger памяти и copies P2

За100 изолированных parse+header-read и100 writer responses4096 измерено0
allocations этих классов. Это не zero-heap всей библиотеки: request body,
header convenience API, query/form maps, application handlers и OS могут выделять.

| Объект/резерв | C3 ABI, compile-only | Срок/причина |
|---|---:|---|
| HTTP head |8192 B|Один buffer на serialized worker, прямой recv |
| Индекс32 headers |512 B|Offsets/views, до callback/send completion |
| Connection slots7 |168 B|Generation/ownership, без тяжёлого head на socket |
| HttpRuntime |160 B|Pointers/config/stop/cleanup state |
| Start subtotal |9032 B (8.820 KiB)|Без allocator/worker/body/maps overhead |
| TcpHttpBackend |56 B|Отдельный object владельца, включая bind address copy |
| Parser / Writer / SessionIo |84 /172 /64 B|Stack/local facade, backing storage отдельно |
| RequestBody POST4096 |4096 B payload|Прежний owned vector; prefetched prefix копируется один раз |

Синхронный payload ответа заимствуется до возврата send; writer не копирует его.
Header read destination — явная bounded copy. Borrowed static DefaultHeaders
больше arena768 сохраняются; новый скрытый outgoing byte ceiling не введён.
Query/form parsing и URL/CBOR decoders не переписаны.

Настоящий C3 GCC14.2, GNU++17, -Og, no exceptions/RTTI, warnings-as-errors:
5/5 common implementation units и sizeof probe compile-only PASS, без SDK include
paths. Согласованный профиль: `local/tcp-p2-c3/og-common/`; соседние вспомогательные
profiles не смешиваются с этими compiler frames.

| Compiler frame, -Og | Bytes |
|---|---:|
| executeHttpConnection после extraction rejection |208 |
| Тот же frame до extraction |1200 |
| sendRejection, только ветка отказа |1040 |
| Small rejected-body drain |320 |
| SessionIo::finish |304 |
| HttpRuntime::run / Exchange::sendResponse |256 /256 |
| Writer prepare / parser parseHead |192 /160 |

Это не сумма всей глубины стека и не аппаратный high-water. Extraction убрала
992 B из frame обычного пути, сохранив отдельный Response для rejection.
Windows Worker metadata24 B выделяется один раз; native CRT/kernel ресурсы отдельно.
У текущего Zig test exe измерено16 MiB virtual stack reservation на worker,
не committed RAM и не расход C3. MCU worker/stack ещё предстоит в P6.

Evidence: `local/tcp-p2-build.log`, `local/tcp-p2-suite.log`,
`local/tcp-p0-host/Testing/Temporary/LastTest.log`, `local/tcp-p2-browser-final/`,
`local/tcp-p2-c3/og-common/`, независимые parser/writer/worker/lifecycle harnesses.
Проверены style и взаимные reviews. SDK TCP link, C3 hardware, Linux runtime,
load и soak ещё не выполнялись; эти результаты не закрывают P3–P7.
P2 закрыт для перехода к P3. Композиция и ограничения описаны в
[руководстве](readme_tcp_http_ru.md); checkpoint — ignored `local/tcp-engine-p2/`.

Финальная проверка перед остановкой на ночь: полная build PASS, CTest28/28 PASS
за9.50s, targeted Uncrustify32 files PASS, git diff --check PASS. Lifecycle stop20ms
в этом прогоне измерен37ms. Scripted fixture вырос с34104 до36152 B (+2048 B)
из-за optional rollback outcome на128 test steps; это host-only fixture, не MCU RAM.
По просьбе владельца остановлено после P2; P3 не начат. Точка на25 октября:
[продолжение](plans/PLAN_20261024_nawserver_tcp-engine-resume.md).

## P3: общий WS wire engine и TCP SSE binding (25 октября)

При возобновлении проверены все49 fingerprints P2: совпали. До новых изменений
свежая полная host-регрессия28/28 PASS,12.30s. Предыдущая точка остановки выше
сохранена как историческая; текущие результаты относятся к P3.

Добавлены переносимые handshake и frame codecs, WS connection поверх узкого TCP,
TCP SSE adapter и синхронизированный registry с slot+generation. Общий UTF-8 helper
выделен из canonical SSE encoder без изменения его семантики. Новый WS engine
не требует Windows crypto/Unicode API или SDK headers. Fragmentation/control,
маскирование, canonical lengths, UTF-8 и close codes проверяются до callback.
Handshake head/index сохраняются один раз при admission; последующие callbacks
видят стабильные views даже после переиспользования HTTP workspace.

WS admission имеет prepare/commit/rollback до101: создаются client и запись
контейнера, но onConnect ещё не вызывается. Commit не выделяет память. Неудачный
или частичный101 закрывает socket без второго HTTP response и без onConnect.
Исходный виртуальный handler сохраняется, подключение не обходит application API.
Глобальный лимит WS и лимит endpoint проверяются независимо до101.

SSE использует прежние encoder/Sessions/QueuedAdapter/Response bridge. Lease
не хранит Request/Response/Next; HTTP→SSE передаётся после callback. Network I/O
не выполняется под registry/producer mutex. Backend не останавливается при
зарегистрированном SSE: сначала stop producers и drain EventSource, затем end.
Local completion подтверждается лишь после фактического освобождения stream.
Известный retained WouldBlock допускает retry при явном stop; ambiguous Fatal
сохраняет ресурсы и возвращает отказ. Negative fixture проверяет этот отказ,
а не имитирует успешный cleanup. TCP adapter допускает повторный start после
полного drain, сохраняя generations; прежний контракт других SSE adapters сохранён.

HTTP и WS имеют одного I/O owner; SSE — отдельного владельца. Producer WS
копирует payload в owned queue и не ждёт сеть. Outgoing deadline начинается при
выборе active frame до readiness wait: отсутствие первого writable события тоже
приводит к timeout. Peer review нашёл этот пробел до gate, regression добавлен.

### Реально выполненные проверки P3

| Проверка | Результат |
|---|---|
| Handshake и независимый Go SHA-1/Base64 oracle |2236540 checks,4096 oracle vectors PASS |
| Frame codec |150464 checks,14009 splits PASS |
| UTF-8 helper/SSE parity |Все1112064 допустимых Unicode scalar values и negative corpus PASS |
| Prepare/commit/rollback admission |2350 checks; prepare2 allocations, commit0 PASS |
| Scripted WS connection |348 checks; owned payload, controls, stale tokens, timeout PASS |
| Concrete SSE lease/retained close |356 checks и отдельный Fatal-retained negative case11 checks PASS |
| TCP SSE worker |807 checks,100 restart cycles PASS |
| Реальный Edge: HTTP/WS/EventSource |18/18 и повтор PASS; JavaScript errors0 |
| Windows x86 length probe |size_t4, high-bit1002/overlimit1009 PASS |
| C3 common object compile |12 product units + sizeof probe PASS, GCC14.2 strict -Og, без SDK/link/runtime |

Заключительная полная host build PASS, CTest **39/39 PASS,23.02s**, включая
усиленный capped integration fixture. Targeted Uncrustify61 C++ files PASS,
git diff --check PASS. Предшествующий полный прогон39/39 PASS27.02s сохранён
отдельно; между ними усилен только integration fixture, product sources заморожены.

Capped integration использует настоящий WindowsTransport с test-only ограничением
каждого read/write до1/2/3/7 B и инъекциями WouldBlock/Interrupted. Одновременно
проверяются HTTP auth/query/binary, fragmented WS/ping и SSE с producer-owned copy;
end при активном SSE, drain/restart и stale token. При лимите WS4 пятый получает503
без callback; независимый endpoint2 также отказывает третьему. Fatal write после
0/1/45/102/128 из129 bytes101 даёт ровно переданный prefix и EOF; каждый раз
admission возвращается и новое соединение успешно. Это функциональная fault
integration, не длительный или предельный нагрузочный тест.

Малые непрочитанные WS/SSE ответы могли попасть в kernel до HTTP: это evidence
mixed live sockets/progress, не доказательство одновременного queued backlog
или реального kernel backpressure. Controlled stalled reader относится к P7.

### Память, allocations и оставшийся P4

Это согласованный C3 ABI compile-only профиль, а не heap/high-water на железе.
Fingerprints всех92 файлов include/src до/после финальной компиляции совпали.

| Резерв/объект | Bytes | Причина |
|---|---:|---|
| HTTP-only startup requested |9259 (9.042 KiB)|P2 +227: runtime/registry close state/wait arrays |
| Startup при WS4 |9355 (9.136 KiB)|Дополнительные wait entries/shared_ptr array, включая4 B array cookie |
| TcpHttpBackend отдельно |68|Постоянный объект владельца |
| WS object + RX4096 + pending4 |5120 usable (5 KiB)|Одна reassembly area, без head на каждый TCP slot |
| Тот же admission с cookie/control/client/set node |5176 requested|Плюс фактические handshake bytes и16 B на header |
| Максимальный admission snapshot |13880 requested (13.555 KiB)|Head8192 + index512, до TX/allocator overhead |
| P3 TX payload, queued+inflight |До16384/client,65536/4 clients|Промежуточные per-message allocations; общий pool16 KiB относится к P4 |
| TcpAdapter SSE отдельно |12960 (12.656 KiB)|Существующее bounded Sessions/queue storage |
| SSE lease с control block |76|На admission, не на каждый write step |
| Canonical SSE pending-close delta |+32 C3 /+40 host|Close serial/flag и ABI padding; полного Step[4] snapshot нет |

Обычный fixture handshake150 B/5 headers даёт5350 usable WS bytes до control/
client/node/TX/allocator; worst-case head не выделяется без соответствующего
реального запроса. WS receive unmask/reassembly выполняются in-place; producer
делает одну payload copy, затем ownership переносится в active frame. Header и
payload отправляются отдельными spans, временного полного frame vector нет.
Изолированный codec decode и hot output не выделяют память. Сохраняемые shared_ptr
control blocks и стандартный set при admission могут выделять: recoverable global
OOM всей библиотеки при no-exceptions этим этапом не обещается.

Compiler frames -Og: обычный executeHttpConnection224 B (P2 208,+16 bindings),
WS opening/message callbacks по1200 B, HTTP rejection1040 B, WS rejection1024 B.
Callback envelope содержит исходные Request80 B и Response952 B; подмена handler
сокращённой семантикой ради размера не выполнялась. Это отдельные frames, не
сумма call chain и не аппаратный stack high-water. QueuedAdapter::poll688 B.

Реальная фоновая SDK HTTPD/C3 regression на pinned IDF5.4.0 PASS352.15s;
отдельная incremental compiledb проверка PASS6.38s. Те же приложение/assets/
sdkconfig/actual flags, что P0: linker Flash889808 B (+1024), static RAM42560 B
(+16), app image890208 B (+1024). Flash delta: .text+992/.rodata+32; admission
unique text+936 B, UTF-8 extraction net+18 B, прочие callsites/layout+38 B.
Endpoint100→104 B вместе с alignment дал .bss+16 B. TCP engine в этот HTTPD image
не подключён; это regression native backend, не SDK TCP build. C3 не прошивалась.

Первичная C3 object проверка обнаружила 32-bit always-false length comparison;
исправлена проверкой старшего бита uint64_t. Сохранены исходная ошибка и свежий
PASS, выполнен отдельный настоящий Windows x86 probe. Три агента проверили
codec/admission/registry/lifecycle друг у друга; блокирующих findings не осталось.

Evidence: `local/tcp-p3-codec/`, `local/tcp-p3-session/`, `local/tcp-p3-sse/`,
`local/tcp-p3-c3-common/`, `local/tcp-p3-idf5-httpd/`, `local/tcp-p3-browser/`,
`local/tcp-p3-transport-integration/`. SDK/link и runtime evidence разделены.
Финальные build/suite/LastTest/style: `local/tcp-p3-final-*-refresh.log`,
`local/tcp-p3-final-style.log`; полный снимок — `local/tcp-engine-p3/`.
P3 закрыт для перехода к P4. Нагрузочные и длительные gate не закрыты.

## P4: bounded WS TX pool и borrowed HTTP body (25 октября)

Один WebSocketTxPool принадлежит HttpRuntime и живёт дольше всех sessions/leases.
Default4×4096 B =16384 B payload backing и4 B occupancy table выделяются до
listener, только при наличии WS routes. Endpoint с большим message limit требует
явно большего block_size; неверные/переполняющиеся размеры отклоняются до I/O.
Никакого heap fallback при полном pool нет. Move-only lease удерживает блок при
queued и active partial output, возвращает после completion/abort ровно один раз.
Пустое сообщение тоже занимает блок. RX и actual handshake snapshot по-прежнему
выделяются только при WS admission, не на каждый TCP slot.

Producer try-lock возвращает Busy; общий pool exhausted — GlobalFull без закрытия
клиента. Individual queued+inflight limit — QueueFull и atomic request-close1013
конкретной сессии через I/O owner. Уже передаваемый frame не прерывается другим
frame: завершается либо socket закрывается по absolute deadline. Pending payload
не выделяется и не копируется при dequeue. Новые enum значения добавлены в конец;
native HTTPD/memory/прежний Windows не получают TCP pool semantics автоматически.

HttpExchange получил optional preparedBody capability: nullopt сохраняет прежний
owned readBody fallback, present empty означает полное пустое тело. Dispatcher
проверяет length/null pointer и не читает fallback при malformed capability.
TCP получает один body buffer размера max_body_size на HTTP worker. Coalesced
prefix копируется в него один раз, остальные recv пишут сразу в конечные offsets.
RequestBody заимствует bytes до возврата callback; owned vector view вычисляется
при обращении и остаётся корректным после move. Custom HttpRequestHandler всё ещё
может читать prepared bytes через readBody. Oversize/incomplete/query/form/binary
и copyBodyTo semantics сохранены. Тяжёлый body buffer не умножен на7 TCP slots.

### Реально выполненные проверки P4

- Финальная build PASS; **CTest42/42 PASS,24.84s**. Первый полный41/41 PASS30.57s
  сохранён отдельно, затем включён runtime storage/failure/SSE ledger harness.
- Реальный Edge **18/18 + повтор PASS**, JavaScript errors0, browser/demo exit0.
  Собственный demo на ephemeral loopback завершился штатно через30s.
- Pool46/session1628 checks,3 независимых повтора PASS; Windows x86 pool46 PASS.
  Детерминированный Busy, global sharing, individual1013, partial active output,
  zero-length, timeout/abort, concurrent producers и retained Closed token после
  уничтожения backend dependencies проверены отдельно.
- Prepared capability55, RequestBody42, scripted HTTP2535 checks/166 exchanges PASS;
  pointer identity, повторное workspace use, binary/NUL, fallback, malformed views,
  incompleteness и custom readBody покрыты. Prepared dispatch0 C++ allocations,
  fallback в этом POST fixture —1 vector allocation.
- Runtime storage harness **17223 checks PASS**: все8 HTTP-only и11 WS4 explicit
  startup nothrow allocation failures доlistener; все6 explicit admission failures
  дают503 до101, без onConnect, затем новый handshake проходит. Live-new baseline
  восстанавливается после каждого cleanup. Ordinary shared_ptr/set OOM не инжектируется.
- Targeted Uncrustify71 C++ files PASS; взаимные source/test reviews без blockers.

Первый socket subset дал6/7: старый WS corpus содержит endpoint8192 при default
pool4096, поэтому корректно получил InvalidArgument при begin. Этот corpus теперь
явно использует4×8192 B pool; проверка default profile и rejection несовпадения
остаётся отдельной. Полный финальный corpus проходит; исходный failure сохранён.

### Allocation/copy ledger P4

Host counters ниже измеряют requested C++ new bytes, включая array cookies, но
не malloc headers/alignment, OS/SDK allocations или task stacks. Runtime harness
использует реальные HttpRuntime/parser/registry/WS session и test-only transport/
static worker; он не доказывает thread scheduling/join/stack costs. Эти стороны
проверяются отдельными настоящими socket/worker suites.

| Фаза host x64 | Explicit nothrow requests | Ordinary new | Requested bytes |
|---|---:|---:|---:|
| HTTP startup |8|0|14135 |
| HTTP+WS4 startup |11|0|30703 |
| WS admission, head145/5 headers |6|2|5793 +64 control/node |
| GET / POST4096 direct body |0 /0|0 /0|0 /0 |
| Fragmented WS4096 RX + pooled echo TX |0|0|0 |
| SSE lease admission |1|1|64 +24 control block |
| SSE preface / encode+publish+send512 |0 /0|0 /0|0 /0 |

SSE измерен через actual ConnectionRegistry/TcpSseExchange/lease и canonical
QueuedAdapter/EventSource, с ручным poll и fixed transport. До callbackComplete
poll не отправляет bytes; после close registration освобождается только с lease.
Это allocation/wire/lifetime evidence, не scheduling/deadline или kernel load test.
Encoder/Frame ownership и bounded Step copies сохраняются; zero new не значит zero-copy.

Точные pointer identity и wire equality проверены тестами. Copy accounting ниже —
по текущим операциям в source, не общий инструментированный счётчик memcpy:

- GET body copy0; POST4096 fixture читает все4096 B прямо в workspace, без prefix.
  В отдельном coalesced corpus копируется только prefetched prefix, до4096 B,
  dispatcher второй полной копии не делает. Fallback других backend сохраняет vector.
- WS incoming fragments unmask/reassembly идут прямо в один RX buffer; outgoing
  WS4096 делает одну producer→pool payload copy4096 B. Header отдельно; сетевые
  short writes не копируют payload в новый frame buffer.
- SSE encode пишет bounded Frame, publish сохраняет owning Frame, next копирует
  до256 B в Step за попытку I/O; pending current Frame также имеет value ownership.
  При partial/WouldBlock повторная Step copy допустима. Эти существующие расходы
  не названы zero-copy и не оптимизировались скрытой заменой lifetime contract.
- Query/form maps, string convenience API, user callbacks и SDK allocations не
  входят в нулевые transport-phase counters; их обычный fallback не отменён.

Согласованный **C3 compiler ABI**,14 product objects +probe PASS,94 include/src
fingerprints совпали. Это не SDK TCP link/runtime heap measurement:

| Объект/резерв | Bytes | Изменение к P3 |
|---|---:|---|
| HTTP startup requested |13395 (13.081 KiB)|+4136: body4096 +runtime40 |
| HTTP+WS4 startup requested |29879 (29.179 KiB)|+20524: тот же4096/runtime40 +pool16388 |
| HttpRuntime / TcpHttpBackend |272 /76|+40 /+8; pool object24 уже внутри runtime |
| RequestBody / Request |24 /92|+12 /+12 для optional borrowed view |
| WS connection / pending entry |992 /16|+16 /+4 для pool lease/overload flag |
| WS object/RX/pending4 usable |5152|+32; RX4096 прежний |
| WS admission known requested |5208|Cookie4/client12/control20/node20; плюс actual head/index |
| Максимальный admission snapshot |13912 (13.586 KiB)|До общего TX pool/allocator overhead |
| SSE adapter / lease requested |12960 /76|Без изменения к P3 |

Default payload ceiling уменьшился с65536 до16384 B (**−48 KiB**); startup/idle
теперь резервирует эти16 KiB заранее, ещё4 KiB добавлены для reusable HTTP body.
Это обмен worst-case RAM и predictability на idle reservation, а не бесплатная
экономия48 KiB при любом числе сообщений. Mutex/occupancy/descriptors и allocator
overhead считаются отдельно. Control payload reserve500 B (3×125 +close125),
вместе с C3 metadata/padding544 B уже входит в992 B connection, не скрытая очередь.

Compiler frames -Og: executeHttpConnection256 B (P3 224,+32), prepareBody64 B,
dispatchHttp1248 B (native P3 matching compiler frame1216), WS opening1216 B,
message1200 B; rejection1040/1024 B. Это не сумма call chain и не hardware high-water.

Фоновая настоящая native HTTPD/C3 IDF5.4.0 SDK regression PASS357.86s,
compiledb PASS5.94s. Те же main/assets/sdkconfig и actual flags; все source hashes
совпали. Linker Flash890026 B (+218 к P3: text+178/rodata+40), static RAM42560 B
(без изменения), image890432 B (+224, alignment). Новые capability/view operations
меняют shared dispatcher/vtables; TCP pool/body workspace не включены в этот image.
SDK TCP и hardware этим не подтверждены; прошивка не выполнялась.

Evidence: `local/tcp-p4-final-{build-refresh,suite,LastTest,style}.log`,
`local/tcp-p4-prepared-body/`, `local/tcp-p4-pool/`, `local/tcp-p4-storage/`,
`local/tcp-p4-browser/`, `local/tcp-p4-c3-common/`, `local/tcp-p4-idf5-httpd/`.
P4 закрыт для перехода к P5. Snapshot — ignored `local/tcp-engine-p4/`.

## P5: настоящий isolated Linux, sanitizers и часовой fuzz (25 октября)

Подключён LinuxTransport: nonblocking/CLOEXEC socket+accept4, локальный pollfd
scratch на64 уникальных fd с разворачиванием duplicate interests, send MSG_NOSIGNAL
без изменения process signal policy. fd0 и fd≥1024 допустимы. Linux close выполняет
одну попытку и освобождает ownership также при errno; retry, способный закрыть
переиспользованный fd, запрещён. Эта policy относится к Linux, не к lwIP.
Private pthread worker использует TLS identity до публикации native handle,
monotonic bounded join и освобождение context только после настоящего thread exit.
Общий CV worker corpus сохранён на Windows. Новая Task/Clock/HAL API не добавлена.

Host CMake отделяет shared runtime от concrete platform sources. Один tcp_main
использует те же routes/assets и явные port/duration/numeric IPv4 bind arguments;
default loopback8081. Прежние Windows-native backend targets сохранены.
Тот же HTTP/WS/SSE/lifecycle/capped corpus перенесён через test-only socket fixture;
ошибка recv больше не выдаётся за EOF. OS aliases не попали в product API.

### Стенд и реально выполненная матрица

Docker Desktop был установлен, но его запуск завершился собственной ошибкой
inference socket. Reset/изменение Docker configuration не выполнялись. Вместо него
создан отдельный WSL2 `NaWServer-P5`, VHDX и image находятся в ignored local/.
Официальный Ubuntu24.04.5 rootfs проверен по SHA256 manifest; GCC13.3, Clang18.1.3,
CMake3.28.3, Ninja и Go1.22.2 установлены внутри этого отдельного стенда.
Kernel6.6.87.2 WSL2/x64, glibc2.39. Это native Linux execution, не только cross-build.

| Проверка | Свежий результат |
|---|---|
| Полная Windows regression |44/44 PASS44.53s |
| Полная native Linux GCC |41/41 PASS10.13s |
| Полная native Linux Clang |41/41 PASS8.43s |
| Linux Clang ASan/UBSan + leak checking |41/41 PASS14.32s |
| TSan реальные worker/WS/SSE/lifecycle/integration |7/7 PASS4.95s; обновлённый relaxed pool oracle отдельно PASS2.25s |
| Linux transport GCC/Clang |6580 checks каждый, fd возвращаются к baseline |
| Linux transport musl ELF runtime |3×6583 PASS,100 accepted-connection restarts |
| Private worker Windows/Linux |637 /633 checks,100 cycles PASS |
| Реальный Edge → Linux demo |18/18 +repeat,20 normal runs PASS |
| Дополнительный browser resource profile |Ещё20×18/18 PASS;60 SSE sessions drained |

ASan/UBSan build сначала не линковался: test vptr instrumentation ссылалась на
RTTI библиотечных private classes, собранных с fno-rtti. Отключён только vptr
sanitizer; AddressSanitizer и остальные undefined checks сохранены. Это tooling
profile, не исправление product memory bug. TSan strong new/delete interceptors
несовместимы с global-new counters fault fixtures; эти allocation tests проверены
обычным/ASan corpus. Для TSan добавлен отдельный pool concurrency oracle без таких
overrides, плюс настоящий многопоточный runtime/socket corpus.

В peer review обнаружено, что первоначальный test ownership guard с acq_rel/release
сам мог создавать happens-before и скрывать дефект pool reuse. Guard переведён
на relaxed: атомарная double-owner проверка осталась, payload synchronization
теперь обязан обеспечить сам pool mutex. Новый native TSan run21080 checks PASS,
8 producers/8000 bounded attempts; moves/payload4096/release/reacquire проверены.
Предыдущий trace сохранён как superseded, не выдан за итоговый oracle.

### Найденные и исправленные реальные различия

**WS close.** Первые оба Linux full suites дали38/39. Decoder отвергал объявленную
длину8193 после4 bytes до чтения4 mask bytes; немедленный native close с unread RX
вызывал RST, и клиент терял Close1009. Аналогично unmasked frame оставлял payload.
Диагностическая копия с удалением только unread tails проходила; canonical тест
сохранил полные8/3/8-byte invalid inputs и строгие assertions.

Теперь после полного WS Close owner делает не более одного shutdownWrite и
incremental raw discard в прежний RX buffer: ≤256 B за readiness, ≤4096 B всего,
absolute deadline50ms. EOF завершает, WouldBlock/Interrupted не продлевают deadline,
fatal/malformed I/O прекращает session. Ни decoder, ни callbacks после Close не
вызываются. Deadline проверяется снова после wait перед recv. Другие соединения
продолжают обслуживаться; ожидания peer в callback или дополнительного buffer нет.
Фактическая reclamation зависит от reactor/scheduler, это не hard real-time50ms
гарантия. При flood сверх бюджета/reset доставка close code остаётся best effort.
Native canonical WS GCC/Clang прошёл, затем полные suites выше; direct scripted
suite2041 checks×3 PASS, новые shutdown/drain branches и stale tokens покрыты.

**SSE diagnostic application.** Первый Linux browser дал17/18 с anonymous Publish409;
повтор прошёл. После добавления observable refusal reasons отдельный20-run batch
выявил ещё и реальный initial connected-event Busy:19 PASS/1 disconnect, при этом
59 regular publish responses200. Причина исходного anonymous409 точно не записана;
он не назван задним числом доказанным Busy. Data/id проверки не ослаблены.

Core try-lock/Busy policy сохранена. SseTest теперь при initial Busy хранит только
owned LastEventId+token+pending flag; owner poll вне HTTP callback кодирует один
temporary Frame и повторяет публикацию. Snapshot защищён mutex; API вызывается
вне app mutex, очистка только того же поколения. Чужой новый slot не затрагивается.
В host и minimal C3 owner loops добавлен poll; C3 tick broadcast остался раз в5s,
а poll вызывается раз в20ms. Stop прекращает poll до drain.

Regular publish сообщает exact409 body `busy` только если никто не принял payload
и встретился Busy; любой Queued даёт200. Browser повторяет только этот отказ,
не более8 попыток с15ms async паузами внутри прежнего7s abort. Accepted payload,
network error и другие отказы не повторяются. Deterministic route injection
проверил один Busy до forwarding, затем один accepted200 без replay; негативы
closed/503/wrong body/network error и8 Busy остаются видимыми failures.
App fixture85 checks PASS: ownership после source overwrite, zero-new poll,
generation replacement, cancel/Closed/fatal и partial broadcast acceptance.

После fix реальный browser18/18+repeat и20 normal runs PASS; в отдельном resource
batch ещё20×18/18 и60 publish200. Там естественно встретился initial Busy1,
который успешно восстановился через poll: active0, begun=completed60, queued_bytes0.
HTTP `/events/close` сам по себе не доказывает drain; completion проверен отдельно.

### Два завершённых часовых fuzz jobs

Запускались одновременно в отдельных systemd units в WSL, DynamicUser и
NoNewPrivileges/ProtectSystem/PrivateNetwork/PrivateTmp. На каждый: CPUQuota100%
(один core), MemoryMax768 MiB/Swap0, TasksMax16, RuntimeMax3700s, file limit16 MiB;
libFuzzer max_total_time3600, max_len16384, timeout5, RSS guard512 MiB, seed20261025.
Эффективные cgroup files/UID/net namespaces проверены, не только принятие команды.
Первый23-second запуск прервался из-за WSL lifecycle и исключён. Bounded WSL client
keepalive удержал distro для полного повтора; profiles/corpus до/после сохранены.

| Job | Inputs | Actual seconds | Coverage/features | Tool peak RSS | Exit/findings |
|---|---:|---:|---:|---:|---|
| HTTP parser/writer/handshake |11430890|3602|654 /1693|261 MiB|0 /0 |
| WS frame/accept/header |127473900|3601|316 /1002|262 MiB|0 /0 |

Crash artifacts0. Семь source/generator fingerprints совпали; current binary hashes
сохранены отдельно, не названы before/after binary-hash proof. Cgroup peak после
GC transient units недоступен; tool peak RSS не подменяет этот отсутствующий metric.
Это safety/metamorphic oracle, не независимая полная RFC model и не нагрузка sockets:
проверяются split/API equivalence, guards, observed event payloads и writer output.
Час без findings не доказывает отсутствие всех ошибок. Opt-in CMake fuzz targets
дополнительно собраны и прошли1000-run smoke каждый; long binaries/corpus не менялись.

### Ресурсы и оставшиеся границы

Короткий native GCC Debug resource run160s,320 samples с500ms interval; HTTP health
≤1/s. Unprivileged demo: CPUQuota25%, RAM128 MiB/Swap0,Tasks16. Observer выявляет
PID reuse/starttime, читает bounded /proc и не посылает signals. При20 browser runs:
sampled RSS3840 KiB постоянно, threads3, fd4→sampled peak6→4. CPU delta5.41s,
average3.39% одного core; это Debug/WSL, не MCU performance measurement. Private
metric548→636 KiB включает private-clean file pages и dirty pages, не равно owned
heap allocations; рост88 KiB не выдан за измеренную утечку или экономию библиотеки.
Для leak acceptance основными остаются live-allocation baseline, fd/slot drain,
ASan/LSan и будущий длинный normal-load профиль. Короткий run не заменяет soak.
Все health samples успешны. Demo штатно завершился: unit inactive, MainPID0/exit0.

LinuxTransport24 B, local poll scratch512 B; GCC-O2 wait own frame576 B (scratch
плюс64 B), без libc call stack. Native pthread stack reservation8 MiB, Windows
executable reservation16 MiB — virtual host reservations, не C3 RAM.
C3 common14 objects+probe compile-only PASS: WS connection1008 B (+16 к P4),
runtime272 B без изменения. Close-drain использует прежний RX, heap reserve0;
readReady own frame128 B, wantsWrite64 B, completeOutput48 B.
Diagnostic SseTest C3 object504→1096 B (+592 B), host Windows792→1424 B (+632 B):
owned metadata вместо4 full Frames. C3 poll own frame1024 B +existing callees,
не total high-water. SDK mutex/TLS allocations не спрятаны в zero-heap claim.

Настоящая background C3 minimal application SDK build IDF5.4.0 PASS398.87s:
Flash946510 B/static RAM46928 B. Это исходный sample с self/response/url fixtures,
нынешними root frontend assets и owner poll, а не прежний размерный baseline.
Его числа не используются как delta стоимости TCP engine. Два неуспешных setup
attempts нового temporary SDK launcher (outside-src/main target) сохранены отдельно;
стандартный main wrapper компилирует оригинальный sample без изменения secrets.
Прошивка не выполнялась, hardware и IDF TCP этим не проверены.

Evidence: `local/tcp-p5-linux/`, GCC/Clang/ASan/TSan build dirs, `local/tcp-p5-fuzz/`,
`local/tcp-p5-linux-transport/`, `local/tcp-p5-worker/`, `local/tcp-p5-ws-close/`,
`local/tcp-p5-sse-busy/`, `local/tcp-p5-resource/`, `local/tcp-p5-c3-common/`,
`local/tcp-p5-c3-application/`. Targeted style82 C++ files и diff-check PASS;
после full suites исправлен только один indentation tab, без semantic change.
P5 закрыт для перехода к P6. Checkpoint — ignored `local/tcp-engine-p5/`.

## P6 — в работе: IDF transport, worker и выбор backend (2026-10-25)

Этап не закрыт: полная SDK-матрица и четыре строки C3 runtime ещё Pending.
На начало P6 аппаратных действий не было; первые текущие результаты C3 ниже. P5 checkpoint сохранён отдельно.

Добавлены конкретный `tcp::IdfTransport` через публичные lwIP sockets и приватный
FreeRTOS worker; общего Task/Clock HAL не появилось. Compile-time выбор HTTPD/TCP
и флаги WS/SSE разделяют списки исходников и зависимостей. Общий TCP runtime
проверен компилятором C3 в четырёх вариантах WS/SSE: 20/20 object compilations PASS
с GNU++17, no exceptions/RTTI и Werror. Это compile-only без SDK/runtime.
После этих изменений Windows повторно: 44/44 CTest PASS, 46.72s.

Worker отдельно собран/слинкован настоящим IDF5.4.0 для C3: PASS261.19s;
узкий повторный compile/relink44.96s, warnings0. Фактический SCons command
подтверждает GNU++17; CMake compdb до PlatformIO overrides не используется как
доказательство итоговых flags. По C3 DWARF Worker16 B, SDK TCB336 B;
запрошенные stacks HTTP6144 B, SSE4096 B, суммарно вместе с двумя Worker/TCB
10944 B (10.6875 KiB). Не включены allocator overhead, lazy lwIP/pthread state;
это capacity, а не измеренный hardware stack high-water. Worker own frames ≤32 B.
После callback выполняется cleanup семафора этой задачи, публикация completion,
private suspension и проверка отсутствия task на любом core перед синхронным
TCB/TLS delete. Timeout сохраняет владение; arbitrary application TLS destructor
и SDK scheduling не прерываются этим timeout.

Для lwIP нельзя перенести Linux close policy: ошибка prepare-delete оставляет
socket выделенным, а nonblocking close при FIN-memory pressure может ждать.
Adapter устанавливает и повторно проверяет SO_SNDTIMEO50ms перед shutdown/close;
SDK TCP poll около500ms и scheduler/mailbox waits идут дополнительно. Это не
обещание hard50ms. При guard failure native close не вызывается; неизвестная
ошибка сохраняет handle, известная memory-pressure допускает явный повторный
cleanup. Нет linger0, private TCP abort или бесконечного retry.

Точный официальный SDK6.0.3 скачан в отдельную ignored environment; archive SHA256
`748b12484402d8a1cb58ba68b7545d2a1f96d36820ab0145e0332c8348ba5ab7`
совпал с release digest. Тексты public APIs/task cleanup и lwIP close проверены
для5.4.0 и6.0.3; нужные semantics совпадают. Source inspection не заменяет
SDK6 build и C3 runtime. Инструменты6.0.3 устанавливаются только для C3 отдельно
от прежних SDK; installed6.1 не выдаётся за6.0.3.

### Первый аппаратный профиль: C3 / IDF5.4.0 / HTTPD

Только исходный app0: image946608 B, [0x10000,0xF71B0), sector erase end0xF8000
внутри app0 end0x290000. Перед записью снова проверены chip/MAC разрешённой C3;
partition CSV сохранён, bootloader/partition/NVS не записывались. Backup и отдельные
post-flash readback/hash passes не выполнялись; только штатная vendor write check.

Свежие on-device results: response89, loopback63, lifecycle30, failed0, running1.
Ignored runner усилен tagged lifecycle/readiness gates: прежний Run-C3 не проверял
lifecycle и мог вернуть success без HTTP-ready. LAN network/WS smoke PASS;
response probe198 checks PASS, включая2/4concurrent clients. Его snapshot:
free177868 B, minimum155532 B, largest114688 B, HTTP stack minimum free1980 B.
Snapshot не является leak/soak evidence, main/SSE stack high-water пока не снят.

Browser сначала дал18/18 и repeat18/18, затем timeout загрузки первой отдельной
negative page. Panic не было. Harness теперь закрывает normal page/context перед
последовательными refusal fixtures: unchanged firmware полный run18+repeat18 и
все5refusal fixtures PASS. Старый failure сохранён; точное исчерпание конкретных
native slots не измерено, firmware fix этим не объявлен. Проверка одновременно
открытых browser contexts остаётся нагрузочным сценарием P7.

Новый scoped minimal SSE probe не подменяет несовместимый integration_device
клиент. Его первый запуск дал read timeout до stop; afterward active0,
begun=completed34, queued0, panic не наблюдался. Эту часть аппаратного профиля
пока не закрываем: уточняется stage и выполняется отдельная доставка/drain/restart
проверка с four-active SSE и WS witness. SDK6 full samples собраны отдельно с проверенным CUSTOM original partition profile; native WS runtime выявил необходимость post-handshake hook, описанную ниже.
### Из чего получилась экономия Flash HTTP-only

Финальный SDK5 refresh подтвердил ту же пару: HTTPD368356 B, TCP335954 B,
разность32402 B (31.6426 KiB); static RAM12040→12064 B (+24 B).
Это lean minimal без Wi-Fi, UI assets, WS/SSE и диагностических unit fixtures;
совпали реальный GNU++17/-Og/no-exceptions/no-RTTI и общий состав routes/handlers.
Исходные первая пара ELF/bin/map/config сохранены до refresh для воспроизводимости.

| Вклад в TCP минус HTTPD | Bytes |
|---|---:|
| SDK http_parser | -16752 |
| SDK esp_http_server | -12131 |
| libc | -9770 |
| NaWServer | +14483 |
| stdc++ | +941 |
| Объединённый пул строк/main attribution | -5764 |
| vfs | -1610 |
| freertos | -1007 |
| Остальные archives | -765 |
| Linker fill | -27 |
| **Итого** | **-32402** |

Восемь новых TCP objects дают17000 B, удалённые native esp_idf/httpd_response
2525 B, common delta+8 B: net NaWServer+14483 B. libc сокращается главным образом
из-за исключения svfprintf10944 B: map inclusion chain native
httpd_response→snprintf→svfprintf; часть компенсирует strerror/system_error.
Объединённый `.rodata` GNU ld приписывает representative main section, поэтому
-5764 B не названы уменьшением application code. 27 B точно подтверждены map
`*fill*`: rodata696→669 B, остальные initialized section fill одинаковы.
Прямой esp-tls dependency исключён из TCP graph, но linked esp-tls contribution
в этой plain-HTTP паре отсутствовал; TLS Flash saving не приписывается.
Подробная арифметика и object ledger: `local/tcp-p6-build-matrix/http-only-flash-ledger.md`.

Firmware image sizes368752→336352 B отличаются на32400 B: image headers/segment
alignment отличаются от четырёх initialized section metric PIO. В настоящем полном
sample с Wi-Fi/WS/SSE/UI/self-tests финальные SDK5 images946608→924592 B,
разность22016 B (21.5 KiB). Здесь printf/snprintf нужны самому sample;
экономию31.6 KiB нельзя переносить на произвольную полную прошивку или runtime RAM.
Evidence текущего блока: `local/tcp-p6-feature-compile/`, `local/tcp-p6-worker/`,
`local/tcp-p6-idf-transport/`, `local/tcp-p6-build-matrix/`, `local/tcp-p6-sdk6/`,
`local/tcp-p6-windows-build.log`, `local/tcp-p6-windows-ctest.log`.
### P6: обновление после первых TCP hardware прогонов

SDK5 compile matrix16 вместе сSDK6 первоначально прошла до runtime; SDK6 native WS
при этом оказался неработоспособен на C3: SDK больше не вызывает обычный URI
handler после handshake. Исправление только в IDF adapter регистрирует SDK
post-handshake callback; SDK6 native WS требует
`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`. Missing-option configure теперь
реально отклоняется. Новая host модель сначала воспроизвела отсутствие open,
после исправления WS-off208 / SDK5 legacy258 / SDK6 post-hook260 PASS. Два native
SDK6 WS/FULL профиля пересобраны: Flash355706/366084 B, staticRAM14064/27080 B;
остальные rows не менялись. Native full SDK6 hardware fix ещё проверяется.

TCP C3/IDF5.4.0: response89, loopback63, lifecycle30, LAN HTTP/WS и198response
checks PASS; browser18+repeat18+5refusal fixtures PASS. SSE probe отдельно:
3 одновременных streams/exactdrain PASS, stop2SSE+WS witness PASS; наблюдались
закрытие старого WS, новый WS echo после restart и serial drained1/active0/
begun=completed32/queued0/errors0. Это проверка2SSE при stop, не объявление4.

TCP C3/IDF6.0.3: response89, loopback63, lifecycle30, LAN HTTP/WS и198response
checks PASS. SSE admission3/exactdrain и stop2+old/newWS witness PASS;
serial drained1/active0/begun=completed46/queued0/errors0. Один browser повтор
раньше завершился generic failure без row diagnostics. Следующий полный run
прошёл, но это не названо исправлением. Бounded repeat с новой диагностикой
поймал другой конкретный симптом: WebSocket binary/NUL timeout на refusal page
(ожидаемый publish refusal был второй, отдельной failing row). Panic нет;
уточняются handshake/frame timestamps. P6 пока не закрыт, failure не скрыт.

Windows network inventory выявила3 established C3 connections существующего
Chrome владельца. Они сохранены. При native max7 тест4SSE+WS+control поверх них
просил9slots, admission4+control —8; этот стенд не был изолирован. Late after-exit
begun counters не доказывают admission до deadline: queued request может быть
принят после освобождения slots. Explicit --clients2 дляstop и --clients3 для
admission учитывают budget; defaults4 tools не понижены. Однако nativeSDK5
stop2 тоже дал timeout publisher, поэтому это отдельный Pending, не выдано за
полностью объяснённый отказ. Chrome соединения не закрывались.

| SDK/backend, одинаковый короткий response probe | Free heap B | Minimum B | Largest B | HTTP stack min free B |
|---|---:|---:|---:|---:|
|5.4.0 HTTPD|177868|155532|114688|1980|
|5.4.0 TCP|145988|126848|114688|2148|
|6.0.3 TCP|151880|131956|114688|2312|

Это snapshots, не leak/soak proof. SDK5 TCP snapshot free меньше на31880 B
(31.1328 KiB); известные TCP startup reserves — примерно13.1 KiB HTTP workspace
и16 KiB global WS TX payload pool, плюс metadata и отличия SDK resources. Полной
атрибуцией net heap delta эти числа не названы. HTTP/SSE requested stacks6144/
4096 B есть и в native profile; нельзя приписывать весь SSE stack толькоTCP.

SDK6 использует picolibc вместо SDK5 newlib. Из linked tasks.c DWARF: TCB336→96 B,
исключено inline xTLSBlock/_reent240 B на offset88. Но SDK6 добавляет libc TLS:
native .tdata16 B, TCP .tdata16+.tbss8 B. Port ALIGNUP16 резервирует16/32 B внутри
requested task stacks; наши pointers добавляют16 B расхода доступного stack
каждойtask. Это не extraheap поверхstack и не доказанная net240 B экономия/task:
libc globals/lazy allocations отдельно не измерены. Main/SSE high-water остаётся
P7 instrumentation, individual compiler frames не заменяют суммарный запас.
### P6: повторная диагностика 25 октября, этап ещё открыт

Свежая итоговая host-проверка: Windows, isolated Linux GCC, Clang и
ASan/UBSan (`vptr` исключён в существующем sanitizer profile) — по46/46 PASS.
Отдельный TSan concurrency subset —7/7 PASS. Попытка собрать весь набор под
TSan дала link conflict custom allocation counters с sanitizer interceptors;
она сохранена как ограничение, а не названа runtime race или full TSan PASS.
Из224 fingerprint records изменился только browser harness, не входящий вCTest;
223 остальных, включая C++/headers/CMake, совпали. Evidence:
`local/tcp-p6-final-host/run-20261025-184330/`.

Native IDF6 после post-handshake исправления реально прошит: response89,
loopback63, lifecycle30/failed0/running1; LAN HTTP/WS и198response checks PASS.
Отдельно admission3/exact drain и stop2+old/new WS witness PASS, serial:
drained1/active0/begun=completed32/queued0/errors0. Native IDF5 финальный source
refresh тоже прошит:89/63/30, LAN HTTP/WS PASS, admission3/drain и stop2+WS PASS,
drained1/active0/begun=completed55/queued0/errors0. Его response probe с явными
двумя клиентами дал78checks PASS. Default probe по-прежнему проверяет2/4 и198
checks; новый `--max-concurrent-clients=2` обозначает отдельный профиль, без
автоматического понижения нагрузки. Попытка default4 при четырёх соединениях
Chrome владельца превысила native capacity7 и дала timeout; она не скрыта.

Последовательный browser corpus воспроизводит неожиданные WS/SSE failures и на
native IDF5/6, и на TCP IDF6. Normal18+repeat18 PASS в отдельных runs не закрывает
полный corpus с пятью refusal fixtures. SDK native сохраняет обычные HTTP sessions,
а shared TCP закрывает обычный HTTP после ответа; native full session table7 при
disabled LRU исключает listener из select. TCP connect и отправка headers в backlog
поэтому не доказывают admission/dispatch на native. Это проверенная по pinned SDK
разница, но не доказанная причина каждого отказа.

Пассивные CDP/NetLog captures различают: незавершённый TCP connect до отправки
HTTP headers; connect+headers без ответа; быстрый reset нового SSE до HTTP headers.
Первый вывод об исчерпании browser socket pool исправлен: незавершённый connect
может ещё не иметь привязки SOCKET к HTTP job. Шесть других активных sockets в
одном capture относились к другим destinations, не к C3. Отсутствие CDP
loadingFinished для status-only fetch само по себе также не доказывает stall:
ограниченные модели с routing и без него завершили20/20 responses/bodies.

Дочитывание health/close response bodies и ожидание WS close проверялись как
кандидаты через JS override только собственного test browser, на неизменённой
прошивке. Повторы снова дали failure: эти кандидаты не объявлены исправлением и
не перенесены в product frontend. Не увеличивались deadlines или socket limits.
Raw NetLog/serial/firmware остаются ignored; обезличенный анализ:
`local/tcp-p6-sse-diagnosis/`.

Отдельный diagnostic-only C3 TCP6 image с неизменёнными HTTP assets прошёл89/63/30.
Скан public socket API во время одного отказа показывал лишь2 sockets (listener
и1 WS), active SSE0, owned2, около151KiB free heap. Это не census PCB/backlog и
не доказательство отсутствия TCP resource pressure. Скан занимает49B static и
собственный224B stack frame, без SDK callees. FIONREAD в данном SDK config
не поддержан; `rx_known=0` не означает пустые receive queues. Продолжается
диагностика фаз native I/O и TCP PCB; P6 до объяснения failures не закрыт.

По уточнению владельца текущие новые fuzz-прогоны ограничиваются10 минутами;
полная длительная программа выполняется после завершения кода. Ранее завершённые
часовые P5 прогоны остаются историческим свидетельством своего source snapshot.

Дополнительный diagnostic-only image добавил operation slots и read-only PCB
census в TCP/IP task; product transport/HTTP assets остались неизменными. Во время
пойманного timeout HTTP-worker находился в обычном wait с age0–20ms, SSE-worker
idle; listener accept counter0 при backlog7, active2 PCB и14 TIME_WAIT. Это не
подтвердило зависший native I/O или заполненный backlog. По DWARF точного C3 ELF
tcp_pcb208B, tcp_pcb_listen76B, netconn52B, tcpip_msg16B;14 TIME_WAIT —2912B
(2.84375KiB) только структур, без allocator overhead. В старом diag2 возможен
unsigned-underflow age при пересечении tick: огромные значения INVALID; diag3
исправляет этот diagnostic edge. Все debug images и их ELF сохранены отдельно.

В собственном Windows Edge подтверждено повторное использование полного TCP
4-tuple: один и тот же local address/port и C3 destination, в одном случае через
100ms; повторное подключение заняло1017ms. В pinned lwIP TIME_WAIT lookup идёт
до listener и не делает нового handshake при таком совпадении. Причинная связь
с конкретными failures пока не объявлена доказанной; подготовлен ограниченный
forced-port-reuse probe, не raw packet flood.

Сравнение через transparent TCP proxy на isolated WSL с ограниченным control
packet capture дало два полных browser PASS, затем на третьем прогоне diagnostic
image21C06639... упал с `tlsf_free: block already marked as free`. Прогон немедленно
остановлен; proxy завершился по180s limit, capture1001 SYN/RST packets/0kernel drops.
Raw pcap/serial остаются ignored. Источник double-free сейчас определяется по
точному сохранённому ELF: это новый открытый failure, а не принятие proxy path или
доказательство дефекта product-кода. Дальнейшие аппаратные прогоны приостановлены
на время crash-разбора. Evidence: `local/tcp-p6-c3-runtime/idf6-tcp-proxy2*`.

## Текущие ограничения

Приоритет владельца уточнён: сначала рабочий MVP на C3; Windows/Linux сейчас
используются для быстрых сборок и поверхностных регрессий. Расширенная матрица и
длительные прогоны откладываются до MVP. В код внесён safer-close: HTTP/WS
сохраняют прежний bounded discard перед одним native close; SSE использует один
registry close без отдельного SHUT_WR. Transport API не подменён, новых буферов
или аллокаций нет. Windows46/46 и Linux GCC46/46 PASS, scoped style9/9 PASS;
контрольный тест со старым core ожидаемо отклонён. Исправленный полный TCP sample
собран под IDF5.4.0 и6.0.3 и реально проверен на C3 в MVP-профиле ниже.
Evidence: `local/tcp-p6-safer-close/`. Старый witness image сохранён, его серии
не выполняются ради расширения исследования перед MVP.

### C3 MVP после safer-close: фактический итог

Проверен текущий root core без diagnostic/witness кода. Два полных TCP sample
(HTTP+WS+SSE) собраны с C++17/no-exceptions/no-RTTI, без HTTPD/esp-tls в graph.
Firmware images: IDF5.4.0 —924448B, IDF6.0.3 —939456B. Каждый image на144B
меньше соответствующего прежнего TCP image; это измеренная разница образов,
а не атрибуция всех Flash sections. Новых retained buffers, fields или hot-path
allocations safer-close не добавляет. Все16 lean profiles проверялись до этого
изменения; текущий MVP refresh —только два full TCP builds.

На разрешённой C3 под обеими SDK свежие on-device результаты: response89,
loopback63, lifecycle30/failed0/running1. LAN HTTP/auth/body/WS0..4096 и пять
переподключений Go-клиента PASS. Response profile явно2 клиента:78checks PASS.
SSE admission3/exact drain, stop2SSE+WS, закрытие старого WS и новый HTTP/WS после
restart PASS. SDK5 stop:begun=completed13/active0/queued0; SDK6 stop до финальной
перепрошивки:begun=completed38/active0/queued0. HTTP callbacks сами server.end
не вызывают. Serial stop gates сохранены в ignored logs.

Отдельный `tests/browser_c3_mvp.cjs` проверяет обычный режим приложения:
один постоянный WS, два SSE и HTTP одновременно. Он не заменяет прежний полный
browser corpus и не скрывает его failures. На C3 в реальном Edge под каждой SDK:

- 20/20 HTTP health и20/20 публикаций;
- 20 text и20 binary WS echoes;
- Каждый из двух SSE клиентов получил все20 событий с точными id/data,
  включая UTF-8, multiline и NUL;
- WS закрыт, active0/queued0 и точные begun/completed delta2 подтверждены stats;
- Panic/assert/WDT не наблюдались отдельным serial observer.

| Обычный browser MVP,20 iterations | IDF5.4.0 | IDF6.0.3 |
|---|---:|---:|
| Scenario duration, ms |7893.7|7725.3|
| Max HTTP health, ms |260.7|114.7|
| Max WS text/binary, ms |140.0/164.7|116.0/74.1|
| Max SSE delivery обоим, ms |147.4|132.8|
| Cleanup, ms |185.0|193.6|

Это максимумы20 samples, не percentiles и не нагрузочный/длительный тест.
Отдельный response2 snapshot: SDK5 free145840/min126684/largest114688/
HTTP stack min free2144B; SDK6 free151740/min133940/largest114688/stack2304B.
Они не доказывают отсутствие утечек за длительное время.

На плате оставлен обычный TCP MVP IDF6.0.3, image03801101..., без диагностики.
После последней прошивки повторены89/63/30 и обычный browser MVP; SSE worker
оставлен запущенным, тестовые WS/SSE clients освобождены. Перед каждой записью
повторялись chip/MAC gate и app0/sector range check; Wi-Fi secrets, partition/NVS
не менялись, backup и дополнительные post-flash readback/hash passes не делались.

Первый полный browser corpus после safer-close прошёл, следующий repeat дал
SSE-coexistence timeout при новом WS. Поэтому полная приёмка P6 остаётся открытой.
Быстрое открытие/закрытие соединений Chrome, портовое TIME_WAIT поведение,
first-PCB-free witness и длительная устойчивость не объявлены исправленными или
проверенными. По приоритету владельца обычный C3 MVP проверен первым; широкие
матрицы, нагрузка/soak и дальнейшее исследование этого edge отложены.
Native HTTPD сохранён; его source path этим cleanup не менялся.

Evidence: `local/tcp-p6-safer-close/sdk5/`, `sdk6/`,
`local/tcp-p6-c3-runtime/idf5-tcp-safer*`, `idf6-tcp-safer*`,
`idf6-tcp-mvp-final*`. Raw serial, device identity и firmware остаются ignored.

### Позднее 2026-10-25: на C3 оставлена полная Showcase TCP-only / IDF6

По отдельному поручению владельца minimal заменена полной `examples/showcase`
с общей Vue UI и 10 WS2812 GPIO2/RMT. Свежая сборка exact IDF6.0.3 и app0-only
flash PASS; image 1 087 232 B, HTTPD/esp-tls отсутствуют в graph/map.
API `info` и UI теперь сообщают фактические SDK и выбранный backend. Добавлен
явный выбор native/TCP в конфигурациях самой демки. Короткая аппаратная проверка:
24 API/assets checks, realtime16/16 (30 секунд WS+2SSE+HTTP), browser6/6,
RGB state и serial без panic/assert/WDT — PASS. Адрес `http://192.0.2.7`.
Детали, память и ограничения — в
[свежем отчёте Showcase](../examples/showcase/docs/readme_c3_realtime_verification_ru.md#2026-10-25-полная-showcase-tcp-only-idf-603).
Это заменяет указанную выше оставленную на плате minimal; P6 reconnect edge и
P7 load/soak остаются открыты. Другие Showcase/SDK/backend комбинации здесь не
объявляются повторно проверенными. Evidence: `local/tcp-showcase-idf6/evidence/`.

- Общие HTTP/WS/SSE и bounded storage проверены на Windows и isolated Linux.
- Для IDF 5 и 6 результаты учитываются отдельно; установленный другой minor
  не заменяет запланированный IDF 6.0.3.
- В начале был только остановленный docker-desktop; теперь отдельный WSL2
  NaWServer-P5 действительно используется для native Linux и sanitizer/fuzz.
- Все четыре C3 SDK/backend профиля реально прошиты и прошли on-device/Go/SSE
  probes. Native SDK6 post-handshake hook исправлен и проверен на устройстве.
  Неожиданные browser WS/SSE failures остаются под диагностикой; P6 не закрыт.
  P7 load/soak не выполнены.
- Authenticated WS unsupported. Deadline входящего неполного WS message пока
  не задан отдельной policy; bounded RX не является доказательством таймаута.
  До P7 требуется явная policy и проверка stalled sender.
- Linux рядом с ioBroker допускается только после isolated gate и проверки
  реальных quotas/health по runbook плана.

# NaWServer: память, жёсткие лимиты и обычные клиенты — 27.10.2026


Дополнение 27.10.2026: исходные результаты этого среза сохранены. Причины REST/WS, исправление и две повторные обычные серии находятся в [отдельном журнале](readme_20261027_tcp_rest_ws_close_ru.md). Перегрузка остаётся отдельным ограничением.

Публичные настройки реализованы без знания о платах, LCD и демо. Окружение, SDK и пакеты не обновлялись. Ветка `codex/http-backend-adapters`, HEAD `632dafdb3575b7f08e1639efaee956f583bfc59e` + незакоммиченные изменения. Commit/push не выполнялись.

**Результат:** ресурсные ограничения и выбор памяти реализованы; полная приёмка обычного REST не пройдена из-за редких dial timeout. Увеличение запаса RAM не считается исправлением этих задержек.

## Публичные настройки

| Группа | Настройки / предел | Поведение |
|---|---|---|
| TCP HTTP/WS | `TcpOptions.buffer_memory`, default64 КиБ | Allocator и жёсткий бюджет зарезервированных массивов на экземпляр |
| SSE | `sse::Limits.buffer_memory`, default16 КиБ | Отдельное размещение и бюджет кадров/очередей |
| ESP-IDF | System / InternalOnly / PsramOnly / PreferInternal / PreferPsram | Только последние два разрешают fallback |
| WS | Новый `max_pending_bytes`, default16384 Б | Учитывает queued + активный частично переданный payload; TCP overflow запрашивает закрытие |
| HTTP / WS / SSE | Существующие byte/count/deadline limits сохранены | Превышение не расширяет очередь/буферы |
| JSON/MCP | Caller-owned workspace | В TCP не добавлены знания о JSON или демо |

Kconfig: `NAWSERVER_TCP_MEMORY_POLICY`, `NAWSERVER_SSE_MEMORY_POLICY`, `NAWSERVER_TCP_BUFFER_BYTES`, `NAWSERVER_SSE_BUFFER_BYTES`; параметры экземпляра имеют приоритет. Служебные блокировки/задачи и память SDK/ОС не переназначаются. Бюджет считает requested array bytes, исключает allocator overhead, небольшие control objects, стеки и чужие выделения. Это не ограничение всей RAM ESP. [API и границы](readme_buffer_memory_ru.md).

## Конфигурации

| Плата | SDK / backend / JSON | TCP / SSE policy | Бюджеты TCP / SSE | HTTP header / body | WS clients / message / pending bytes |
|---|---|---|---|---|---|
| C6 | 5.5.6 / TCP / bounded | System / System | 65536 /16384 Б | 8192 /4096 Б | 4 /4096 /16384 |
| S3 финальная | 5.5.6 / TCP / bounded | PSRAM-only / PSRAM-only | 65536 /16384 Б | 2048 /4096 Б | 4 /4096 /16384 |
| S3 контрольная | То же приложение, исправление alignment включено | System / System | Те же | Те же | Те же |

Общий WS TX pool4×4096 Б; SSE до4 клиентов, pending3, индивидуальный queued budget2304 Б, общий8192 Б. TCP registry: C6 7, S3 21; sockets SDK:10/24. Обработчики приложения, Wi-Fi и LCD не входили в размещение библиотеки. Только приложение S3 выбрало строгую PSRAM в своём sdkconfig-профиле.

## Обычный REST: 1 / 2 / 3 клиента

Каждый этап60 с: один последовательный запрос/с на клиента, без backlog; 9 summary и1 INFO на10 запросов. Без assets/MCP/WS/SSE и без прикладных POST/PUT. Тот же клиент: dial/header deadline2 с, total4 с. Источник192.0.2.3, прямой LAN без proxy. Отдельный observer читает memory/tasks примерно раз в10 с; пользовательские фоновые подключения сохранены. Это короткая проверка, не ночная приёмка непрерывной автоматизации.

| Плата | Клиентов | HTTP ответов | Ошибок / timeout | p95 успешных, мс | Max успешных, мс | Min sampled internal free, Б | Min largest block, Б |
|---|---:|---:|---:|---:|---:|---:|---:|
| C6 | 1 | 56 | 2 /2 | 302.222 | 1043.977 | 188988 | 167936 |
| C6 | 2 | 120 | 0 /0 | 111.457 | 368.03 | 187432 | 167936 |
| C6 | 3 | 176 | 2 /2 | 104.329 | 488.442 | 187464 | 167936 |
| S3 | 1 | 56 | 2 /2 | 87.91 | 740.152 | 78951 | 32768 |
| S3 | 2 | 120 | 0 /0 | 70.616 | 119.478 | 82207 | 40960 |
| S3 | 3 | 178 | 1 /1 | 99.603 | 857.246 | 78883 | 34816 |

Все ошибки финальных REST-этапов — dial timeout; boot IDs внутри каждого этапа не менялись, health/info после каждого этапа200/200. p95/max выше относятся к завершённым ответам, не включают неудачные двухсекундные dial. Ни одинаковое число ошибок, ни низкий p95 не локализуют причину в Wi-Fi. В одном снимке C6 RSSI−78 dBm; также сохраняется общий синхронный HTTP/WS owner. TCP пока использует Connection: close: каждый REST-запрос создаёт новое TCP-соединение, keep-alive не реализован. Предсказуемость длительного REST остаётся отдельным незакрытым пунктом.

## HTTP-посещение UI: отдельно от REST

Каждый этап30 с: каждый клиент один раз скачивает найденные JS/CSS, затем читает status/info. Это HTTP-сценарий посещения, **не браузерный E2E и не непрерывная WS/SSE UI-сессия**.

| Плата | Клиентов | HTTP ответов | Ошибок / timeout | p95 успешных, мс | Min sampled internal free, Б |
|---|---:|---:|---:|---:|---:|
| C6 | 1 | 30 | 1 /1 | 100.796 | 187096 |
| C6 | 2 | 64 | 0 /0 | 130.612 | 185900 |
| C6 | 3 | 94 | 0 /0 | 194.283 | 187348 |
| S3 | 1 | 31 | 0 /0 | 158.976 | 77315 |
| S3 | 2 | 63 | 0 /0 | 220.349 | 77507 |
| S3 | 3 | 91 | 1 /1 | 229.671 | 82115 |

## System и PSRAM на исправленном S3

Контроль и PSRAM используют один исходный код с явным выравниванием, одинаковые лимиты и REST3×1 запрос/с. Порядок этапов и радиосвязь различались; разницу задержек нельзя приписывать политике памяти. System может уже размещать16-КиБ TX pool в PSRAM из-за SDK malloc threshold и alignment overhead.

| Размещение TCP / SSE | HTTP ответов / ошибок | Min sampled internal free, Б | Min largest block, Б | Sampled PSRAM free min, Б | SDK сумма минимумов регионов, Б |
|---|---:|---:|---:|---:|---:|
| System /System | 178 /0 | 51807 | 31744 | 5377016 | 10684 |
| PSRAM-only /PSRAM-only | 178 /1 | 78883 | 34816 | 5350772 | 32052 |

SDK `min_free_bytes` суммирует исторические минимумы разных heap regions, наступившие в разное время. Ночные96 Б не означали доказанный одновременный остаток96 Б; исправлены пояснения прежних отчётов. Sampled free/blocks снимаются примерно раз в10 с и могут пропустить короткий пик. Резерв после cleanup и стеки находятся в финальных snapshot; отсутствие утечки по этим коротким этапам не доказывается.

## Перегрузка и отрицательные проверки

Перегрузка:30 с, 4 логических клиента (2WS+2SSE), HTTP pool3, cadence250 мс, непрерывный reload JS/CSS и MCP list/echo/invalid. Это не обычные1–3 REST-клиента. WS-счётчик показывает завершённые точные обмены, не все попытки.

| Плата | HTTP | Завершённых WS echo | SSE events | Ошибок / timeout | Recovery |
|---|---:|---:|---:|---:|---|
| C6 | 279 | 100 | 59 | 3 /3 | 200/200 |
| S3 | 224 | 88 | 56 | 2 /2 | 200/200 |

| Проверка | C6 | S3 |
|---|---|---|
| HTTP body4097 при cap4096 |400, отказ |400, отказ |
| Заголовки больше настроенного cap |431 |431 |
| Лишний WS при четырёх допустимых |503 после3 наших +1 фонового |503 после3 наших +1 фонового |
| WS message4097 при cap4096 |Код1009 не получен, read timeout4 с: проверка не пройдена |Последняя диагностическая попытка1009; предыдущая также имела timeout |
| Малый byte budget / allocator fault / concurrent quota / rollback |Host deterministic PASS |Host deterministic PASS |

Одиночный успешный повтор WS1009 на S3 не отменяет предыдущий timeout. Жёсткие пределы объёма проверены в state-machine/host-тестах; максимальная задержка сетевого закрытия на платах не объявляется принятой. Общий HTTP/WS owner может задерживать WS; не добавлялись reentrant callbacks из HTTP write. [Исходный анализ](readme_tcp_load_findings_ru.md).

## Исправление выравнивания и история попыток

Реорганизация heap проявила ошибку S3 MCP: capability malloc даёт только4-byte alignment, а caller-owned JSON workspace требует max_align_t. Все MCP-запросы возвращали503. Теперь ESP-IDF allocator библиотеки гарантирует выравнивание, а демо явно использует heap_caps_aligned_alloc для своего workspace. Транспорт не знает о JSON. На исправленных C6/S3 и System-S3 MCP list/read/UTF-8/invalid-preserves-state/start/status/stop прошли; эксперименты после проверки выключены.

Ранние артефакты находятся в `local/memory27/pre-alignment/`; их обычные REST/UI successes не являются полной приёмкой firmware с неисправным MCP. Сохранены первоначальные C6 REST failures и отдельный quiet repeat1/2/3 без ошибок до последнего alignment build; они не отменяют ошибки финальной серии. Первый limits probe ожидал неверные413 и header cap2048 для C6: это ошибка harness, исправлена по действующему контракту (body400, C6 headers8192). Все raw JSONL оставлены, пороги timeout не ослаблялись.

## Проверки сборок

| Проверка | Результат |
|---|---|
| Windows host |48/48 CTest PASS, включая quota/failure/alignment/lifecycle и четыре байта alignment в SDK stub |
| Linux через существующий WSL runtime |47/48 общего CTest; oracle harness пытался запускать ELF из Windows без emulator. Те же4096 oracle vectors вручную в WSL PASS; после alignment12/12 целевых проверок PASS |
| Go load tool |go test и go vet PASS; REST-only fixture проверяет отсутствие assets/MCP/WS/SSE |
| C6/S3 TCP IDF5.5.6 |Собраны, прошиты с chip/MAC/range gate и обычной vendor verification; финальные ELF совпали |
| S3 HTTPD IDF5.5.6 |Собран; этой фазой на плате не проверялся |
| C3 TCP IDF6.1.0 minimal |Собран; физического C3-прогона не было |

| Профиль | Target / backend | TCP/SSE policy | BIN, Б | data+bss, Б |
|---|---|---|---:|---:|
| 52 | esp32s3 /TCP | 0/0 | 1534208 | 62728 |
| 53 | esp32c6 /TCP | 0/0 | 1328720 | 64552 |
| 54 | esp32s3 /TCP | 2/2 | 1534224 | 62728 |
| 55 | esp32c3 /TCP | 0/0 | 970704 | 38136 |
| 56 | esp32s3 /HTTPD | not-set/0 | 1553136 | 62664 |

Конфигурации и точные SHA256:

- Профиль52: CONFIG `f3e154e253b75219facbadd52aa48bdf476ae739b19cb1d66725f419e25911f6`; BIN `e120e5ed0bf9b763534dab11370777fd49f9d26ad22eb176a9ed047257a4ac77`; ELF `6b58ce60f5c6df52a5954ea696a3e5a6724488c48632d8dfebf1ba39f1bbb797`.
- Профиль53: CONFIG `c909f2769ab13867a02d796557abb65b5e47d3c41e287651c03b2e6237bd4223`; BIN `888c772a5020a0772d22457b085bf94c1382be6d01c2da658fa53d14de57caa1`; ELF `10fd7272cef9b88a0714b6b3d677a5fcf0a2d79e8bda091ebea262d033052f1c`.
- Профиль54: CONFIG `7831155131271d5c4c0fead3912c9021c1a3c2b4530d4e8a976c9ee3ba72e60e`; BIN `8268d000b5da732b13d6d03351e9bd7237917631489041784df6b989debf5f0c`; ELF `587f3cda7886bc57d99f1241b9e1718267f52753e8b8602d37c9c8d8a42acd85`.
- Профиль55: CONFIG `554f8383e9d7cb3c5e9d2db22548176e0e8770a350529a58b5e05e12bb708bdf`; BIN `855f49b1f17ccbd50234335e583bb0b8ca8361cf09a2d8f3104ac41a17830bc2`; ELF `13bcc91bcada905523332c5f0dcfbac2bfde03d514c1cd0f18513f5f796da236`.
- Профиль56: CONFIG `80edc8e2bebf9e3f5b8d28ef193fb4675eb3fc661e1e8f6afef130b57cec6db6`; BIN `938c59f57d5afb7d8b33d4080f605125967edc5dd0390a533265123272874496`; ELF `093f86dd6ec3b49b624a655ea316ae19f722493b63932171a006625c934f4e66`.

Статическая RAM — data+bss, не runtime heap. Полный журнал конфигураций и linker maps: [журнал Flash/RAM](readme_build_memory_log_ru.md). Локальные исходные результаты: `local/memory27/runs-summary.json`, `firmware-manifest.json`, JSONL и UART; кадры до alignment сохранены отдельно.

## Финальное состояние и следующие действия

На платах оставлены C6 профиль53 и S3 профиль54: TCP + bounded + MCP, S3 buffers PSRAM-only, C6 System. Финальные probes прошли, прикладные эксперименты выключены. Собственные нагрузочные процессы/observer/serial monitor завершены; пользовательские вкладки, сеть ПК и локальный монитор стенда не менялись.

Следующая техническая задача — локализовать редкие TCP dial timeout обычного REST и задержку WS close, затем повторить те же профили и отдельную длительную REST-проверку ночью. Не считать увеличение RAM исправлением TCP/Wi-Fi, не подменять обычную приёмку перегрузочным тестом или удачным повтором.

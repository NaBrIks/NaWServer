# Showcase: WS + SSE и авторизация на физической C3

Проверка 19.10.2026. Новый отчёт дополняет [обновление UI](readme_refresh_20261019_ru.md)
и заменяет его отметку «не прошивалось» для текущей прошивки.

## Устройство и запись

Перед обеими записями повторно определена ESP32-C3 rev0.4 на COM10. MAC совпал с
ранее проверенным устройством. Identity и сырые логи остались ignored в local/.
Бинарная таблица разделов Showcase совпала по SHA-256 с проверенной minimal.
Записывался только app0 с 0x10000; bootloader, partition table, NVS, EEPROM и
LittleFS не записывались. Flash backups и erase-all не выполнялись.

Локальный ignored Wi-Fi header взят из уже работающего examples/minimal.
Первая прошивка подтвердила HTTP/CBOR/WS/SSE/auth, затем браузер выявил ошибку UI.
После исправления заново собраны общие assets и C3; повторная идентификация и
vendor hash verification перед/после записи — PASS. Boot/IP — PASS.

Прошивка на момент исходной WS/SSE/auth-проверки (заменена обновлением
[живого управления и кнопок](readme_live_controls_ru.md)):

- Проект `showcase до переименования`, версия приложения `0.1.0-showcase`, ESP-IDF 5.4.0.
- Размер firmware.bin: **1 034 272 bytes**.
- SHA-256: `f315c20084732582825f3f9b3713e0c00a3bfd682c05ddd14b99d2ae6b5830b6`.
- Запись `[0x10000,0x10c820)`; секторное стирание vendor `[0x10000,0x10d000)`.
- Static RAM 61 568 bytes; flash sections 1 033 866 bytes.

После этой проверки установлено обновление живого управления; minimal обратно не возвращался.

## Исправление, найденное только на устройстве

Перенесённый модуль подсказок вызывал `crypto.randomUUID()` при загрузке модуля.
На localhost это работало, на обычном HTTP LAN-адресе C3 функция недоступна:
`TypeError: crypto.randomUUID is not a function`. Vue не доходил до mount, страница
была пустой. В локальной копии ui-tooltip.js добавлен fallback для ARIA-id.
Это идентификатор DOM-подсказки, не токен доступа или сессии. Обход защиты браузера
не использовался; исходный UI.TechnicalShell и core библиотеки не изменялись.

После исправления реальная HTTP-страница открылась; проверены работа подсказки,
диалога, локальных иконок и тем. Два browser locator-прогона остановились на
нажатии задачи через DOM-адаптер CUA; кнопка работала через live AX. Окончательный
harness выполнен с одним AX-адаптером нажатия `httpd`, остальные действия —
`tab.playwright`. Это ограничение автоматизации не меняло код task inspector.

## Сеть на окончательной прошивке

`frontend/tests/device-realtime-smoke.mjs`: **16 PASS**. Запущен read-only сценарий
с WS, двумя SSE-подписками (публичной и защищённой), HTTP и binary/NUL echo.
30 HTTP-итераций и связанные проверки заняли **54 580 ms**; во время прогона был
также открыт браузер платы. Получены:

| Поток | Получено |
|---|---:|
| WS summary | 54 |
| WS echo.response | 30 |
| SSE public diagnostic | 54 |
| SSE private diagnostic | 54 |
| HTTP summary | 30 |

Ошибок и смены boot_id не было. Проверены source=device, ESP32-C3 / одно ядро,
heap>0, возрастающие seq/uptime, реальные задачи и ненулевые stack margins.
Task sample в нагруженном протокольном прогоне содержал 8 задач; полный UI
отдельно загрузил **11**. Задержки этого смешанного прогона не являются latency SLA.
Два SSE + WS скрипта вместе с SSE/WS/HTTP браузера приближаются к штатному пределу
7 TCP sockets HTTPD. Лимит ядра не менялся; четыре слота SSE не обещают четыре
браузера со всеми потоками одновременно.

HTTP Basic: без credentials 401 + WWW-Authenticate, неверный пароль 401,
правильный fixture 200, недостаточно прав 403. Защищённый SSE отвергнут до открытия
без Basic и передавал настоящие события при корректных credentials.
Фикстура `demo / showcase-only`, permissions=1; admin требует 2.

`frontend/tests/device-smoke.mjs` на окончательной прошивке: **24 PASS**. Включены
task pages, source/device/C3, байтовый WS echo, SHA-256 всех пяти device assets
(CSS, JS, font, HTML, MIT license) против актуального device manifest. Режим RGB
не включался; цепочка и нагрузка оставлены выключенными.

## Браузер с настоящими данными

`runDeviceUISmoke`: **17 PASS** через CUA, непосредственно с HTTP C3:

- Нет пульта/сценариев эмулятора, виден `ESP32 · устройство` и source=device.
- Реальные CPU, RAM, uptime, boot ID, FreeRTOS задачи, httpd runtime/stack.
- Native EventSource получает новые события; WS и SSE одновременно online.
- SSE продолжает поступать при отключённом WS; WS echo работает при выключенном SSE.
- Матрица 401/401/200/403/401 проходит на core Route, с его настоящим Basic challenge.
- Защищённый SSE получает диагностический JSON с source=device.
- SSE-журнал, Dark/EN, локальные Tabler glyphs и диалог на plain HTTP работают.

Во время финального UI-прогона: CPU около 4–5%, свободная RAM около 145 КиБ,
11 задач. Это конкретные наблюдения, не фиксированные значения. Из task sample:
main min stack 556 B, httpd 1972 B, sse_lab 3184 B. Это реальные minima с момента
старта; длительная нагрузка и worst-case stack не проверялись.

Результаты и raw evidence — ignored `local/c3-*.json`, `local/c3-*.log`,
`local/screenshots/c3-ui-verification.json`, `c3-auth.png`, `c3-overview.png`.

Локальный .NET Host после обновления assets также перезапущен штатным helper;
102 прежние проверки модели/HTTP/WS/SSE/auth/assets PASS. локальный монитор стенда остаётся
работать; запись физического устройства переименована с minimal в Showcase через
API, адрес проверен. COM10 после записи освобождён.

Authenticated WS пока не поддерживается ядром: HTTP/SSE auth не объявляется
защитой WS handshake. Не проверялись TLS, длительный soak, физическое свечение,
электрические сигналы и работа других ESP32 targets.

## Воспроизведение

Из examples/showcase:

```powershell
# Только после свежей проверки конкретного C3, исключительного доступа и сборки:
./tests/Flash-C3.ps1 -Port COM10 -ExpectedMac VERIFIED_MAC
# Fresh IP взять из ignored local/c3-flash-state.json:
cd frontend
node tests/device-realtime-smoke.mjs --url http://DEVICE_IP --out ../local/c3-realtime.json
node tests/device-smoke.mjs --url http://DEVICE_IP
```

Flash-C3.ps1 дополнительно сверяет таблицу с ранее проверенной minimal и пишет
только app0; при отсутствии этого локального артефакта или несовпадении прекращает
работу. Browser harness экспортируется из tests/device-ui-smoke.mjs; при CUA
нажатие task inspector можно передать отдельным live AX callback.

## 2026-10-25: полная Showcase, TCP-only, IDF 6.0.3

Это свежая сборка и аппаратная проверка полного `examples/showcase`, а не minimal
и не повторное использование результатов выше. На разрешённой C3 оставлена эта
прошивка; свежий адрес — `http://192.0.2.7`.

### Изменено

- Одна таблица маршрутов и прежние CBOR/middleware/handlers работают через общий
  `HttpServer`. Конкретный backend выбирается Kconfig: TCP или native IDF HTTPD.
  SSE получает соответствующий адаптер. Закрытие WS из синхронного callback
  использует заимствованный `WebSocketExchange`, без native HTTPD FD в приложении.
- PlatformIO `c3` сохраняет native HTTPD, `c3_tcp` выбирает TCP. Оба PIO-профиля
  закреплены на IDF 5.4.0. Для native IDF 5.4.0/6.0.3 отдельные команды и каталоги
  приведены в README. Полная демка требует WS и SSE; это не HTTP-only приложение.
- `/api/v2/info` сообщает `firmware.http_backend` из фактического Kconfig и
  `firmware.idf_version` из app descriptor. Общий UI показывает эти значения в
  паспорте; при отсутствии backend metadata у старого устройства показывает `—`.
- Для закреплённого `led_strip 3.0.1` добавлен узкий SDK6 compatibility include
  `esp_heap_caps.h` только при компиляции этого компонента. Его исходники и версия
  не менялись; первоначальная ошибка компиляции сохранена в evidence.

### Реально проверено

Фоновая сборка exact IDF 6.0.3, C3, 2 jobs, GNU++17/no-exceptions/no-RTTI — PASS.
App descriptor сообщает `v6.0.3-dirty`; это сохранённое фактическое значение SDK,
а не подмена его названием профиля. В graph и linker map нет `esp_http_server`
и `esp-tls`. 36 C++ units и 118 source fingerprints проверены; assets совпадают
с замороженной версией. Образ 1 087 232 B, SHA256
`11650AB4076CD29ABC0E0F0A62286D41DEFD62B84474D3BB8E7B5F2FEB263A67`.

Перед записью повторно определены текущий порт, ESP32-C3 и разрешённый MAC.
CSV и partition binary совпали с ранее проверенной таблицей. Записан только app0:
байты `[0x10000, 0x119700)`, sector erase до `0x11A000`, внутри app0 до `0x290000`.
Vendor write/verify и загрузка PASS. Bootloader, partitions и data не записывались;
backup и дополнительные post-flash readback/hash passes не выполнялись.

- `device-smoke` без `--rgb`: **24 проверки PASS**, включая actual C3/CBOR,
  task pages, хеши всех production assets по локальному manifest и binary/NUL WS.
- Realtime: **16/16 PASS**, 30 секунд одновременно WS, public SSE, private SSE
  и HTTP. Получены 31 WS summary, 30 binary/NUL echoes, по 31 событию каждого SSE,
  30 HTTP requests. Один boot, advancing snapshots, backend `tcp`, реальная задача
  `naw_tcp_http`, положительные stack margins.
- Headless Edge: **6/6 PASS**, полная страница, HTTP/WS/EventSource online,
  advancing SSE, реальные CPU/RAM, фактические SDK/backend в INFO и RGB controls.
  JavaScript errors и request failures — 0; screenshots просмотрены.
- Read-only `/api/v2/strip`: RMT, GPIO2, 10 pixels, supported/enabled true,
  solid, brightness 20%, driver_error пуст. Настройки/драйвер через API не менялись.
- Отдельный serial observer сопровождал все эти проверки: panic/assert/WDT — 0.
  Собственные тестовые клиенты завершены, COM освобождён.

### Память и ограничения

Frontend build/typecheck и штатный asset packer PASS. Device assets gzip 108 172 B,
на 116 B больше предыдущих 108 056 B. Это Flash assets, не retained RAM buffer.
Изменения metadata не добавляют runtime pools или KiB-scale буферы приложения.
ELF: DRAM data 17 692 B + BSS 46 320 B = 64 012 B; это секции, а не весь heap/stacks.
В конце realtime snapshot: free heap 117 092 B, min 99 328 B, largest 102 400 B.
Stack min free: HTTP 1 840 B, SSE 2 496 B, main 1 640 B, showcase_io 3 496 B.
Это значения короткого сценария, а не оценка длительной утечки.

Новые frontend metadata tests 2/2 PASS. Общий frontend suite 10/12: две проверки
требуют отсутствующие S3 dist/assets; S3 не собиралась в текущем C3-only scope.
PlatformIO config parsing и scoped diff check PASS. Другие комбинации обновлённой
Showcase/SDK/backend в этом этапе не пересобирались и не прошивались.

Физическое свечение, SPI driver switching, нагрузка и soak здесь не проверялись.
Этот результат не закрывает ранее открытый P6 edge с частыми Chrome WS reconnect
при SSE и не заменяет P7. Полный старый browser corpus не запускался под видом
этого короткого browser smoke. Следующий этап общего движка остаётся в его плане.

Evidence (ignored): `local/tcp-showcase-idf6/evidence/` — build/final source+map,
flash range/state, realtime/browser/strip results и serial observer. Полная демка
оставлена работающей на C3 с TCP-only/IDF6, RGB on/RMT/20%.

> Актуальное обновление 19.10.2026: [новый UI, SSE, auth и результаты](readme_refresh_20261019_ru.md).
> Ниже сохранены результаты предыдущей версии; её аппаратные проверки не подтверждают новую прошивку.
# Проверки локального showcase — 18 октября 2026

## Текущий результат: CBOR v2, нагрузка и светодиоды

- .NET 10 SDK 10.0.401, Vue 3.5.22, TypeScript 5.9.3, Vite 7.1.12, cborg 6.1.3, System.Formats.Cbor 10.0.11. Версии закреплены; глобальные SDK/инструменты не менялись.
- `dotnet build simulator/Showcase.sln`: PASS, 0 warnings / 0 errors. Пять проектов: Contracts, Model, Host, Tests, Assets.
- `npm run build`: строгий TS и production Vite PASS.
- Showcase.Tests: 88 проверок модели/CBOR/HTTP/WS/ресурсов PASS. Детерминизм, task IDs/runtime/watermark, pause/step/reset, модель памяти, страницы и snapshot conflicts, π/автозавершение/stop, pulse 200/1000, core/priority, LED bounds/default cycle, RGB solid/chase/rainbow/off/brightness/bounds, CBOR double/uint64/byte string, отказ duplicate/trailing/indefinite/text keys, auth/MIME/version/body/frame limits, Unicode, восстановление после ошибок, SHA-256 identity/gzip.
- Edge / Playwright: 63 проверки PASS. π через UI, блокировка второго старта, автоматическое завершение, остановка, минимум 25 точек и четыре резких перехода SVG для видимой гребёнки 200/1000 мс. LED 20/5000 мс и 0/100%, выключение и возврат 1000/50. RGB: десять красных пикселей, один активный бегущий, радуга, выключение. Режим без simulator capability не загружает SimulatorPanel chunk и не обращается к /sim.
- Обзор/tasks/experiments/lab/events проверены на 1440/1024/768/390/320 px без горизонтального переполнения документа. Тема cream, HTTP/WS echo и Basic, offline/reconnect, лента событий. Необработанных ошибок браузера 0; внешних сетевых запросов 0.
- Скриншоты desktop/tasks/mobile/experiments/RGB и `ui-verification.json`: ignored `local/screenshots`. График и RGB визуально просмотрены; найденное растяжение checkbox исправлено, браузерный прогон повторён.

## Размер

Финальные gzip-ресурсы общей страницы: JS 52875 B + CSS 5415 B + HTML 463 B = **58753 B / 57.38 КиБ**. Локальный пульт имитации отдельно: **3471 B**. Полный локальный набор **62224 B**. Это размеры ресурсов, не firmware binary и не потребление RAM.

`asset-manifest.json` содержит полный набор; `device-manifest.json` и сгенерированный `main/assets.hpp` исключают SimulatorPanel JS/CSS. Общая production-сборка одна; при capability simulator_control=false нет загрузки исключённых файлов. Нагрузочные опыты и управление LED/RGB являются прикладной частью общей страницы и остаются в device assets. В firmware не входят node_modules, TypeScript/Vite/Playwright и исходные maps.

## Границы результата

Сервер работает только на 127.0.0.1:8788, старт/остановка через Start-Showcase.ps1 / Stop-Showcase.ps1 с проверкой пути и времени процесса. Проверки запускались последовательно, поскольку меняют общую модель. HTTP/WS настоящие, CPU/память/задачи ESP32 синтетические; небольшое вычисление π на хосте не является benchmark.

Плата здесь не прошивалась, COM-порты не открывались. Основной чат выполнил аппаратную проверку новой C3; результаты приведены в последнем разделе. Владелец подтвердил 10 WS2812/NeoPixel, DIN GPIO2. Прежний GPIO8/active LOW относится к виртуальному опыту SuperMini, наличие этого LED на новой плате не подтверждено.

Прикладной C3 firmware adapter подключён к ByteView request.body(), подготовлены реальные π/pulse и WS2812 RMT GPIO2. Руководство передачи: readme_device_ru.md. Ядро этим чатом не менялось. Данные локального эмулятора не подтверждают точность таймингов, работу RMT/WS2812, физических GPIO или статистики FreeRTOS. S3 здесь только синтетический профиль, его firmware не собиралась.

Ранние отчёты 42/44 проверок и 45021 B относятся к прежнему JSON v1; текущий результат выше их заменяет. CUA runtime ранее недоступен; воспроизводимые UI-проверки выполняет project-local Playwright с установленным Edge.

## Проверки интеграции C++ / CBOR

tests/Test-Native.ps1 на уже имеющемся task-local Zig 0.14.1: 53 C++ проверки PASS; затем 2 обратные .NET проверки PASS. Используется тот же main/wire.cpp и pinned vendor TinyCBOR, а не другой decoder. Проверены команды и binary/NUL из .NET, доступ к полям после возврата parse и переиспользования стека, вложенный WS data map, packed RGB, каждый усечённый префикс WS fixture, malformed/duplicate/trailing/indefinite/wrong-type/invalid UTF8, output overflow и неверное число map полей. Native uint64 max, π double и бинарные NUL выходят из TinyCBOR и читаются .NET без потерь.

Первая полная C3-сборка успешна: 524328 B flash, 33496 B static RAM. Эта промежуточная сборка ещё не включала окончательную локальную Wi-Fi конфигурацию. Актуальный финальный результат ниже. -fstack-usage для приложения: command816 B, ws_message1056 B, parseMap496 B, encode_summary320 B. Это собственные frames функций, не сумма всего HTTPD call stack и не runtime high-water mark. HTTPD stack ядра 6144 B; буферы output/echo/producer по4096 B статические. Нужен аппаратный watermark после запуска.

Исследование неполной локальной MSVC установки прекращено без установки/изменений; host проверки используют существующий Zig. SDK/vendor Python во время штатного PlatformIO build — существующий build dependency; агентские скрипты Python не создавались/не запускались.

## Финальная передача основному чату

Финальная `pio run -e c3 -j 2` SUCCESS (ESP-IDF 5.4, espressif32@6.10.0), включает актуальный игнорируемый Wi-Fi header, исправленный parser и последний общий UI. Flash usage 981792 B / 2621440 B; static RAM 48064 B / 327680 B. firmware.bin 982192 B, SHA-256 `1e645fac6a03694d1d7339265e5b0b74b53faca33dc74759d5dc0571e6ceddf3`. Device gzip58753 B; simulator-only3471 B. Предыдущие меньшие firmware numbers не относятся к этой финальной Wi-Fi сборке.

Файлы firmware/UI передавались основному чату с фиксацией на время аппаратного прогона. Этот чат не открывал serial, не прошивал и не обращался к IP платы. Load/RGB disabled at boot. Результаты отдельного аппаратного прогона showcase приведены ниже; отчёт minimal на showcase не переносится.

Компилятор выдал прежние warnings ядра (deprecated query/form helpers, unused logging tag) и предупреждение legacy partition littlefs/subtype0x82. Ядро и таблица разделов этой задачей не менялись. Firmware не объявляется zero-heap: помимо static RAM существуют SDK/HTTPD/RMT allocations и две созданные один раз task stacks. Пики heap, runtime stack watermarks и влияние сборщика статистики измеряются отдельно.

## Аппаратный результат основного чата — 18 октября 2026

Источник: сообщение основного чата по завершении его аппаратного прогона. Этот чат проверку не повторял и платой не управлял. Проверен именно showcase artifact SHA-256 `1e645fac6a03694d1d7339265e5b0b74b53faca33dc74759d5dc0571e6ceddf3`, а не прошивка minimal.

- Flash: PASS, выполнена основным чатом на согласованной C3. Разметка совпала с minimal; NVS не стиралась. Новая резервная копия не делалась по отдельному указанию владельца.
- Boot: PASS, `showcase до переименования 0.1.0-showcase`.
- IP acquisition: PASS. Адрес устройства, сетевые сведения и сырые serial/network logs остаются в игнорируемой локальной области основного чата.
- Go hardware protocol check: PASS. source=device / C3, strip supported GPIO2 / count10, 401 без Basic, solid red20% с readback `0x330000`, rainbow с более чем пятью цветами, off и summary после команд.
- `frontend/tests/device-smoke.mjs` без `--rgb`: 20 hardware checks PASS. Реальные страницы задач, SHA-256 всех трёх device assets, binary WS summary и echo с NUL.
- Edge/Playwright: живая страница `/#experiments` загружена, screenshot сохранён основным чатом локально. Это отдельное подтверждение загрузки device UI, не полный повтор 63 эмуляторных browser checks.

Нагрузка на плате не запускалась; π/pulse timing, CPU под нагрузкой, длительный прогон и stack/heap watermarks пока не проверены. Цепочка оставлена выключенной. Подтверждены RMT readiness, команды и readback; видимое свечение, правильность физического цвета и все десять пикселей должен подтвердить владелец. Аппаратный freeze снят основным чатом; последующие прошивки по-прежнему выполняет только он.

## Edge WebSocket: исправление лимита заголовков

Read-only проверка живой платы обнаружила отличие от Node smoke: штатный Edge получает `431 Request Header Fields Too Large` при каждом WS handshake; HTTP остаётся online. CDP показал около512 B только строк HTTP headers (User-Agent, Origin, extensions и другие стандартные заголовки). В исходной конфигурации `CONFIG_HTTPD_MAX_REQ_HDR_LEN=512`. Основной чат независимо подтвердил 431 по HTTPD serial diagnostics. Адрес и сырые headers/logs в этот документ не переносятся.

Контрольный Edge с коротким диагностическим User-Agent на той же прошивке подключился: HTTP online / WS online, 46 принятых кадров, socket errors0. Это подтверждает parser ceiling, а не ошибку CBOR или request.body(). Диагностический браузер не отправлял LED/нагрузочные команды; изменение User-Agent не предлагается как пользовательское решение.

По согласованию с основным чатом изменены только showcase sdkconfig.defaults и активный sdkconfig.c3: `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`. Сгенерированный config/sdkconfig.h подтверждает1024; URI остаётся512, body/frame4096. Core, firmware sources и Vue assets не меняются. Требуется пересборка/обновление основным чатом и повтор WS в обычном Edge; до этого стандартный browser WS не считается прошедшим аппаратную проверку.

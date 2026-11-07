> Работа возобновлена владельцем 19.10.2026. Текущий результат и оставшиеся аппаратные проверки: [отчёт](../readme_refresh_20261019_ru.md). Прежняя остановка ниже — исторический статус.

# Продолжение showcase на работе

Статус: остановлено владельцем до следующего указания. Дата фиксации: 2026-10-18, продолжение планируется 2026-10-19. Этот документ — актуальная точка входа; предыдущий большой план содержит историю и устаревшие промежуточные статусы.

## Где остановились

Рабочий проект: C:/Git/NaWServer, приложение examples/showcase. Основной TPMS не менять. Firmware/core API принадлежит основному чату; эта работа — приложение showcase, Vue UI, .NET emulator. Владелец остановил оба чата до завтра, локальная сборка остановлена вместе с дочерними compiler-процессами. Автоматическое продолжение/прошивка не запланированы.

На согласованной C3 основной чат уже проверил прежнюю showcase-прошивку: flash/boot/IP, HTTP/CBOR/auth, реальную статистику/tasks, общий UI, RGB-команды/readback. Нагрузка не запускалась. Аппаратный протокольный smoke: 20 PASS; RMT readiness и readback не подтверждают видимый цвет без владельца.

Последний успешно собранный и проверенный hardware artifact: firmware.bin982192 B, SHA-256 `1e645fac6a03694d1d7339265e5b0b74b53faca33dc74759d5dc0571e6ceddf3`. Flash981792 B, static RAM48064 B. Его лимит заголовков ещё512. Оставшийся в .pio firmware.bin — старый артефакт; не считать его сборкой исправления1024.

## Первая задача: WS в обычном браузере

Подтверждённая проблема: штатный Edge показывает HTTP online, WS offline. WebSocket handshake отклоняется `431 Request Header Fields Too Large`. Стандартные заголовки браузера занимают около512 B ещё до прочих байтов запроса, а CONFIG_HTTPD_MAX_REQ_HDR_LEN был512. Основной чат независимо видел431 в HTTPD diagnostics. Node handshake короче и проходит.

Контроль: Edge с коротким диагностическим User-Agent на той же прошивке получил WS online, 46 кадров, 0 socket errors. Это доказательство причины, не пользовательское решение. CBOR/body accessor здесь не виноваты.

Уже исправлено на диске:

- examples/showcase/sdkconfig.defaults: CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024 — переносимая конфигурация для другого компьютера.
- Локальный игнорируемый sdkconfig.c3: то же1024.
- Сгенерированный .pio/build/c3/config/sdkconfig.h успел подтвердить1024, но сборка остановлена владельцем до завершения.
- URI остаётся512, body/frame4096. Core, firmware sources и production assets ради этой правки не менялись.

Следующие шаги:

1. Проверить git status и текущую конфигурацию на рабочем ПК, прочитать AGENTS.md. Не подменить исправление старым sdkconfig.c3. Секреты не выводить.
2. Завершить C3 build с явным -e c3. Проверить SUCCESS, реальные header1024 в сгенерированном sdkconfig.h, размер и SHA-256 нового firmware.bin. Наличие файла само по себе не доказывает успех новой сборки.
3. Передать готовность основному чату. Прошивку выполняет он после возобновления владельцем; перед flash заново определить разрешённую плату/порт на этом компьютере. Не использовать вчерашний COM/IP автоматически. Не делать erase NVS/whole flash; backup policy согласует основной чат с владельцем.
4. После обновления открыть обычный Edge с обычным User-Agent. Подтвердить HTTP online и WS online, отсутствие431/reconnect loop, поступление summary и binary echo. До этого исправление не считать аппаратно подтверждённым.
5. Повторить read-only device-smoke (20 проверок) и проверить SHA-256 трёх общих assets. Записать build, flash, boot, IP acquisition, protocol и visual checks отдельно; адреса и сырые serial/network logs держать локально вне Git.

Сборка на текущем компьютере (на рабочем адаптировать только путь уже имеющегося toolchain cache):

```powershell
$env:PLATFORMIO_CORE_DIR='<repo>/local/pio-core'
pio run -d examples/showcase -e c3 -j 2
```

Platform espressif32@6.10.0 / ESP-IDF5.4. Не менять глобальные установки как побочный эффект. Firmware image, sdkconfig.c3, managed_components, .pio и wifi_secret.hpp игнорируются. Main/wifi_secret.hpp подготовлен основным чатом локально, на работе его автоматически не будет. Не коммитить credentials и не копировать их в отслеживаемую документацию.

## Потом: подтвердить физические светодиоды

Владелец подтвердил: другая C3, 10 WS2812/NeoPixel, DIN GPIO2. Firmware: GRB/RMT10MHz/noDMA. GPIO8 новой платы не подтверждён и не используется; обычный LED capability=false, его UI скрыт. RGB и нагрузка при старте off, brightness20.

- После явного согласования аппаратного показа проверить solid red10–20%, зелёный/синий, все10 пикселей, rainbow/chase/off. Убедиться в физическом цвете/порядке пикселей с владельцем.
- POST подтверждает настройки; pixels отражает последний успешный output worker. Для readback ждать100–200 ms или bounded retry. Readback сам по себе не доказывает физический свет.
- Не смешивать результат с прежним обычным GPIO8 LED SuperMini. Регуляторы20..5000 ms /0..100% обычного LED остаются опытом эмулятора до подтверждения платы.

## Затем: нагрузочные задачи на плате

Реальный код подготовлен, но пока не запускался аппаратно:

- π: core0 / priority2 /5000 ms, промежуточный результат250 ms, автоматическое завершение и ручной stop.
- Pulse: on200 ms /period1000 ms /duration10000 ms, график CPU с выборкой100 ms должен показать гребёнку.
- Проверить HTTP/WS отзывчивость во время расчёта, фактические CPU/task runtimes, stop/restart, отсутствие watchdog reset. В активной нагрузке предусмотрен1-tick yield примерно раз в10 ms; это не обещание ровно100% CPU.
- Снять min-free heap и stack high-water marks до/во время/после, проверить стоимость 1 Hz task collector. Собственные compiler frames command816 B, ws_message1056 B, parseMap496 B — не измерение полного HTTPD стека.
- Детали задач ограничены32 элементами, HTTP page<=8; snapshot409 при смене коллекции допустим, UI повторяет следующий опрос. Не обещать другие ESP32 targets: S3 пока только синтетический профиль.

Нагрузку не запускать автоматически при возобновлении; первый приоритет — нормальный browser WS и видимое управление RGB.

## Что уже работает и проверено

- Одна production Vue3/TS/Vite UI-сборка, кремовая тема; .NET10 emulator и firmware используют общий CBORv2 с прямыми UTF-8 именами полей (20.10.2026).
- request.body() разбирается в HTTP callback, workers получают только bounded typed params; ядро и CBOR dependency разделены.
- Разделённые main/wire/runtime/telemetry, настоящие heap/FreeRTOS показатели, π/pulse и WS2812 output worker.
- Общие device assets58753 B gzip, simulator-only3471 B исключены из device-manifest и firmware header.
- 88 model/protocol checks на локальном эмуляторе; 63 Edge UI checks; 53 C++ wire checks +2 обратные .NET interop checks; 20 hardware smoke checks основного чата. Обычный Edge WS с512 не прошёл — это текущий первый дефект.
- На дату остановки локальный .NET emulator оставлен запущенным; на другом компьютере запускать Start-Showcase.ps1, это не обращается к плате.

## Известные ограничения и дальнейшие идеи

- connections details endpoint501, capability=false: core не предоставляет нужные session details. Summary count доступен.
- WS TX counters — попытки queue/send, не подтверждённая доставка; текущий void core API может отбросить кадр при overflow. max_ws_clients4 — целевой предел, не гарантированный прикладной admission guard. Изменения ядра согласовывать с основным чатом.
- Authenticated WS и SSE недоступны. Basic demo/showcase-only — открытая fixture, настройки RAM only.
- Legacy warning littlefs/subtype0x82 сохранён вместе с исходной таблицей разделов; разметка при первом flash совпала с minimal. Не исправлять erase/reset-ом.
- Драйверная архитектура выбора устройства/способа управления записана как [идея](../ideas/IDEA_20261018_showcase_control-drivers.md), вне текущего исправления.

## Навигация и перенос на работу

- [Контракт CBOR](../readme_protocol_ru.md).
- [Передача на C3 и команды запуска](../readme_device_ru.md).
- [Проверки и границы подтверждённых результатов](../readme_verification_ru.md).
- Native interop: tests/Test-Native.ps1 -ZigPath <existing-zig.exe>, без установки глобального compiler. Vendor TinyCBOR подтягивает штатный C3 configure/build.
- Hardware smoke: frontend/tests/device-smoke.mjs --url http://DEVICE_IP, по умолчанию только чтение; --rgb требует отдельного решения владельца/основного чата.

Состояние Git при остановке: локальная ветка master, содержимое независимого NaWServer ещё untracked; remote не настроен. Этот план записан в дереве репозитория, коммит/push этим чатом не выполнялись. На рабочем ПК файлы сами не появятся: нужен согласованный перенос проекта либо отдельная подготовка Git/remote/commit/push. Не включать в перенос секреты, build outputs и сырые аппаратные логи.

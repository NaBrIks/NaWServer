# План: диспетчер ESP32, единый UI и .NET-симулятор

Дата: 2026-10-18. Статус: локальная .NET/Vue-демонстрация реализована и проверена; firmware adapter ожидает готовности основного ядра. Этот план заменяет направление раннего Go-черновика, но не удаляет и не меняет его код.

## Решения и границы

Владелец выбрал ASP.NET Core под полноценную Visual Studio, диспетчер ESP32 и пульт имитации. Основная цель — показать систему и возможности веб-библиотеки, без переноса TPMS/BLE/LCD-продукта. Единственные исходники HTML/JS/CSS и единственная production-сборка раздаются и микроконтроллером, и .NET. Нет двух версий страницы, серверной HTML-генерации или режима, в котором браузер сам подменяет ответы случайными числами.

Последнее рекомендуемое направление frontend: Vue 3, TypeScript, Composition API, SFC, Vite; обычный CSS и SVG/Canvas. Владелец утвердил этот стек; frontend/src и simulator содержат его реализацию. Компонентность нужна для независимого развития и перестановки панелей. На ESP32 попадают скомпилированные статические ресурсы; Node/Vite/TypeScript работают только на компьютере разработчика. Библиотека Vue локально входит в bundle, CDN не используется. Не брать по умолчанию Nuxt/SSR, Blazor WebAssembly, SignalR, Bootstrap, большие UI/table/chart/icon-пакеты. Router/Pinia и сторонние графики добавлять только при конкретной пользе и измеренном размере; исходно достаточно hash-навигации, небольшого typed store/composables и своих SVG/Canvas-графиков.

Ограничение «маленькие инструменты на Go» из общих правил переопределено прямым указанием владельца для этого симулятора: он на .NET/C#. Python по-прежнему не используется для собственных скриптов. До согласования миграции ничего не удалять из Go-черновика.

Изменения только examples/showcase/** и при необходимости tools/showcase/** отдельной библиотеки <repo>. Общие src/agents/docs, исходный TPMS, COM5 и плата — вне изменений. Новый core API согласуется и пишется в основном чате. Текущие owner ADR о static composition, next pipeline и простых local-device limits учитывать после их реализации; не сохранять legacy ради старой демки.

## Что можно делать независимо от ядра

1. Уточнить прикладной контракт docs/readme_protocol_ru.md: идентификаторы, единицы, доступность полей, HTTP/WS, ошибки, лимиты, тестовые примеры.
2. Написать .NET-модель устройства и сценарии, реализующие контракт. Показывать все запланированные функции, но маркировать источник как simulator и device_status как planned до интеграции. Эмуляция функции не доказывает её существование на плате.
3. Разрабатывать Vue/TS UI против настоящего HTTP/WS .NET-сервера; визуальные сценарии и автоматизированные проверки выполнять без платы.
4. После готовности core реализовать firmware adapter и диагностический collector в самом примере, выполнить те же контрактные тесты. Только аппаратная проверка подтверждает память/тайминги/лимиты и показания контроллера.

Таким образом ждать готовности C++-ядра для UI и эмулятора технически не требуется. После утверждения владельцем реализация UI и локального эмулятора возобновлена; C++-ядро и firmware не менялись.

## Структура проекта (предлагаемая)

- frontend/src/contracts — TS-типы и проверка входящих данных, разделение DTO и отображения.
- frontend/src/transport — один HTTP/WS client, same-origin URL, reconnect и ошибки.
- frontend/src/state — ограниченные текущие snapshots, браузерная история и фильтры.
- frontend/src/features — overview, tasks, memory, connections, lab, events, simulator; общий набор компонентов table/chart/status.
- frontend/dist — одна production-сборка, используемая обоими серверами; запрет независимых ручных копий UI. Упаковка для firmware проверяется хешами manifest.
- simulator/Showcase.sln и проекты Host, Model, Contracts, Tests. У модели нет зависимостей от ASP.NET/WS; Host содержит endpoints и BackgroundService; SimulatorController управляет сценариями.
- main — ESP-IDF adapter, сбор диагностических снимков и раздача тех же dist-ресурсов из flash. Без новой auth/storage/transport абстракции в src библиотеки.
- docs — контракт, сценарии, запуск/отладка Visual Studio, результаты с разделением local/build/hardware.

Предпочтительный target .NET 10 (локально найден SDK 10.0.401); совместимость установленной Visual Studio и workloads проверить перед реализацией. Не выбирать установленный .NET 11 RC автоматически. Точную версию frontend toolchain и зависимостей закрепить lock-файлом при создании проекта; глобальные инструменты не обновлять. C#-сервер: Kestrel, System.Text.Json, штатные HTTP/WebSocket, DI и BackgroundService; без большого framework над протоколом. Тестовый framework и прочие dev dependencies не входят в firmware.

## Диспетчер

- Обзор: чип/ревизия/ядра/аппаратные возможности; прошивка/SDK/build; uptime/reset reason; Wi-Fi/сервер; сводные CPU/память/соединения.
- Задачи: устойчивый id, имя, состояние с расшифровкой, приоритет, привязка к ядру при доступности, CPU за окно и накопленный runtime, minimum stack reserve. Поиск, сортировка, выбранная задача и ограниченная история; не менять идентичность строк при сортировке.
- Память: free/min-free/largest-free-block по отдельным capabilities-пулам; internal/PSRAM при наличии. Не суммировать пересекающиеся области и не выдавать boot-relative минимум за текущий остаток стека. Для high watermark проверить единицы ESP-IDF и явно документировать.
- Веб-сервер: маршруты и политика доступа, публичные показатели, активные соединения/сессии, сообщения/байты/ошибки/отказы и отключения. Время roundtrip измеряет browser; pending/queued/delivered не смешивать.
- Лаборатория: GET/query, form POST, JSON где поддерживается прикладным endpoint, static/gzip/MIME/headers/redirect/buffered response, HTTP auth, public WS text/binary/echo/broadcast. Рядом результат и короткий серверный пример на актуальном API.
- События: ограниченная лента с фильтрами, timeline, decoded/raw/hex; содержимое только как текст. Никаких паролей, cookie/token/Authorization, дампов NVS/eFuse/памяти или локальных секретов.

Задачи: CPU за интервал из дельт счётчиков, первая/невалидная выборка — unavailable, а не 0. Указать нормализацию для multicore; runtime ticks не называть миллисекундами без частоты/конверсии. Настоящие доступные поля collector должны быть описаны capabilities. Нет данных/выключен collector/не поддерживается/нет доступа/потеря связи — разные состояния.

## Пульт имитации

Та же UI-сборка содержит панель; она доступна по runtime capabilities только на локальном симуляторе. Её административные endpoints существуют только у loopback Host и отсутствуют на firmware. Скрытие кнопки не является защитой API.

- Профили C3 и условного другого устройства: чёткие бюджеты RAM/число ядер/наличие PSRAM; не принимать их за hardware acceptance.
- Состояние сохраняется между тиками: постоянные task ids, монотонный runtime, согласованные CPU/memory/network counters.
- Нормальная работа; нагрузка HTTP/WS; снижение свободной памяти; малый stack reserve; слабый RSSI; потеря HTTP/WS и восстановление; malformed/oversized сообщение; auth отказ; превышение лимита клиентов.
- Управление run/pause/step/reset, seed, скоростью simulation time. Сценарий, seed и выбранный профиль воспроизводимы. Повторный reset меняет boot_id.
- Отдельный debugger-friendly режим: пауза отладчика не маскируется под аппаратную неисправность. Длительные callbacks не удерживают общий model lock; concurrent WS send сериализуется на соединение.
- Различать модель ошибки/нагрузки и измеренный результат работы ESP32. Синтетическая статистика всегда подписана.

## Бюджеты (цели, не измеренные результаты)

- Все HTML/CSS/JS/SVG/favicon/fonts и библиотеки локальны; внешних URL, аналитики, CDN и удалённых шрифтов нет. Системные шрифты и компактные SVG. Offline-тест блокирует любой внешний запрос.
- Цель для полного deploy-набора <=100 KiB gzip; контрольный потолок 160 KiB gzip. При превышении сначала отчёт по составу и уменьшение зависимостей; изменение бюджета согласовать. Это предполагаемый бюджет Vue-версии, не текущий замер.
- Предварительно сжатые ресурсы передавать из flash с верными MIME/Content-Encoding; не распаковывать их в heap устройства. Финальные bytes ресурса/flash/RAM проверить на C3. Не класть исходные maps, npm packages, SDK или dev server в firmware.
- Отдельно считать frontend bytes, firmware binary, пик/минимум RAM, stack collector и транспортные буферы. Малый gzip не означает дешёвую диагностику.
- Текущий transport ceiling 4096 bytes body/frame. JSON-диагностику разбивать на ограниченные страницы; проверять размер в UTF-8 после сериализации, не число символов. Буферы и количество tasks bounded. Лимиты уточняются после реализации ядра.
- Сводка по умолчанию раз в 2 s; подробные задачи — только при открытой панели, не чаще 1 Hz. Браузер хранит до 120 точек на ряд и до 200 событий. Не добавлять replay/history в ядро.
- Типичный профиль 2–4 WS-клиента. Max TCP connections отдельно от WS. Не изображать невнедрённые limits API работающими; emulator может воспроизводить проектируемую политику с явной маркировкой.

## Приёмка и этапы

[ ] Зафиксировать точный v1 контракт, JSON fixtures и общие негативные случаи; отдельные intended/implemented возможности.
[ ] Создать Visual Studio solution и .NET-модель; структурированные сценарии, HTTP/WS и tests.
[ ] Создать Vue/TS UI, layout для desktop/tablet/mobile и независимые feature components.
[ ] Проверить byte-identical assets обоих серверов, offline/no external, UI budget и hash manifest.
[ ] Проверить сортировку/tasks identity, deltas/reboot/stale/unavailable, русский текст, text/binary/echo и ограничение памяти browser/model.
[ ] После стабилизации нового API написать device collector/adapter; C3 build с pinned isolated PIO cache, без upload.
[ ] Владелец/основной чат координирует flash/boot/IP/protocol/hardware tests отдельно.

Результаты предыдущего черновика: Go tests/vet прошли; gzip assets созданы. C3 build и headless browser были прерваны владельцем, полного PASS нет. Эти результаты не относятся к ещё не написанной .NET/Vue-версии.

## Рантайм-источники

Проверено в локальных заголовках ESP-IDF 5.4 и официальной документации:
- esp_chip_info: model/revision/cores/features. Название development board из этого не следует; board_label задаёт приложение, optional.
- esp_app_get_description: project_name/version/idf_ver/build metadata, заложенные при сборке. Смысл project version задаётся PROJECT_VER/version.txt/build configuration.
- esp_get_idf_version: строка SDK, которым собрано приложение.
- esp_reset_reason и системные heap APIs: runtime-диагностика. Tasks/runtime stats зависят от настроек сборки; недоступность описывать явно.

Источники: https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c3/api-reference/system/misc_system_api.html и https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32c3/api-reference/system/app_image_format.html . Это ссылки документации для разработчика, не зависимости UI.

## Исполнение 18 октября 2026

Готовы frontend/src (Vue/TS), simulator/Showcase.sln (Contracts/Model/Host/Tests), Start-Showcase.ps1 и Stop-Showcase.ps1. Реальные функции и ограничения описаны в README и docs/readme_verification_ru.md. Разработчик выбрал кремовую тему по прямому поручению владельца: использованы только значения палитры Cream из C:/Git/SeMaps/editor/src/styles/editor.css; код SeMaps и его диаграммы не переносились и не менялись.

Проверено: строгий TS + Vite build; .NET solution build без warnings/errors; 42 model/HTTP/WS/asset checks; 44 UI checks в отдельном Edge, включая cream, offline/reconnect и mobile. Deployment gzip payload 45021 bytes, три ресурса + manifest. Проверки UI/HTTP выполняются последовательно, поскольку сценарии меняют общую модель.

Исходный Go/vanilla/C++-черновик сохранён отдельно от frontend/dist и не мигрирован в новый library API. Следующая фаза — после готовности основного кода: реализовать device adapter/collector, подтвердить capability полей и limits, собрать C3, физическую проверку выполнить по координации владельца. План пока не закрыт целиком.

## Идеи дальнейшего развития

- [Драйверная архитектура управления: выбор устройства и способа управления](../ideas/IDEA_20261018_showcase_control-drivers.md). Идея владельца; вне текущей реализации.


## Обновление 18 октября: CBOR и прикладные опыты

Предыдущие предложения JSON v1 и прежние замеры выше — исторический этап. Текущий контракт: docs/readme_protocol_ru.md, CBOR v2 /api/v2, прямые UTF-8 имена полей (решение владельца 20.10.2026); JSON fallback отсутствует. Общий UI включает нагрузочные задачи, обычный LED и WS2812. Локальный пульт имитации выделен в отдельные JS/CSS и исключён из device-manifest.json и assets.hpp.

Локально реализованы π с промежуточным результатом, pulse 200/1000 мс, bounded duration/stop/core/priority, график 100 мс, LED 20..5000 мс / 0..100%, WS2812 10 RGB GPIO2 (solid/chase/rainbow, цвет/яркость/период). CPU синтетический; это не hardware benchmark. Владелец сменил плату и подтвердил WS2812/NeoPixel DIN GPIO2; прежний GPIO8 новой платой не подтверждён.

Основной чат владеет src и raw body/copyBodyTo API. Здесь ядро не меняется, плата/COM5 не используются. После получения проверенного API подключить прикладной firmware adapter и отдельно согласовать аппаратную проверку. Драйверная архитектура остаётся идеей дальнейшего развития.

Обновление API: основной чат сообщил о готовности request.body() -> ByteView и copyBodyTo(pointer/array/std::array/vector), BodyCopyResult. Raw view действует только до возврата callback. Документация: ../../../docs/readme_request_body_ru.md относительно showcase. Следующая задача — подключить этот API в adapter; ядро и аппаратная проверка остаются у основного чата. Текущий локальный результат: 88 model/protocol/asset + 63 browser checks PASS; device gzip 58740 B, simulator-only 3471 B.


## Интеграция C3 showcase

Подключены новый request.body() и статический Route/makeChain API. main теперь разделён на HTTP bootstrap, wire codec, runtime workers и telemetry collector. Реальная нагрузка π/pulse вне HTTPD, измеренный CPU по IDLE runtime, задачи/heap 1 Hz, WS summary 10 Hz при опыте/анимации. WS2812 GPIO2/10/GRB/RMT, цепочка и нагрузка off при старте, brightness20. GPIO8 новой платы не подтверждён и не используется. Подготовлен frontend/tests/device-smoke.mjs, здесь на плате не запускался. Основной чат координирует прошивку.

PASS: C3 первая сборка, 53 C++ wire interop + 2 обратных .NET проверки; финальные assets 58753 B device gzip, simulator-only3471 B. Подробности запуска и ограничений: ../readme_device_ru.md. Реальные hardware boot/IP/HTTP/CBOR/WS/RGB и timing остаются отдельной аппаратной приёмкой, их не считать проверенными по эмулятору.

Финальная сборка C3 SUCCESS: flash981792 B, static RAM48064 B, firmware.bin982192 B, SHA2561e645fac6a03694d1d7339265e5b0b74b53faca33dc74759d5dc0571e6ceddf3. Код/ресурсы зафиксированы и переданы основному чату. Этот чат не прошивает; аппаратные результаты пока pending.


## Аппаратный smoke завершён основным чатом

Проверен финальный showcase artifact SHA2561e645fac6a03694d1d7339265e5b0b74b53faca33dc74759d5dc0571e6ceddf3: flash/boot/IP acquisition PASS, Go HTTP/CBOR/auth/RGB readback PASS, 20 device-smoke checks PASS (tasks/assets/binary WS), живая experiments UI загружена через Edge. RGB оставлена off, нагрузка не запускалась. Видимое свечение/физический цвет/все пиксели и реальные π/pulse timings/watermarks остаются pending. Freeze снят; прошивки выполняет основной чат. Сеть/IP/сырые логи в отслеживаемые документы не перенесены. Детали: ../readme_verification_ru.md.

Диагностирован Edge WS handshake431 при header ceiling512 (Node handshake проходит). Контроль Edge с коротким UA: WS online46 кадров/0ошибок. Showcase config повышен до1024, URI/body/frame и core/assets не меняются. Основной чат выполняет обновление и проверку обычного браузера после готовности сборки.


## Остановка владельцем

Работа обоих чатов остановлена до завтра. Сборка header1024 прервана, старый firmware.bin не является её результатом. Актуальная точка входа: [план продолжения на работе](PLAN_20261018_showcase_resume-work.md).


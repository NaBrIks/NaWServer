> Обновление 19.10.2026: SSE реализован отдельным текстовым потоком JSON, сохраняя CBOR v2 для HTTP-команд/телеметрии и WS. Текущие результаты — [в отчёте](readme_refresh_20261019_ru.md).
# Прикладной контракт showcase v2 — CBOR

Дата: 2026-10-18. Реализован локальным .NET-эмулятором и общим Vue-интерфейсом. C3 firmware adapter подключён через raw body API; основной чат подтвердил flash/boot/IP, HTTP/CBOR/WS, task pages, общие assets и RGB command/readback. Нагрузка и видимое свечение ещё требуют отдельных проверок. Версия относится к приложению, не к C++ API NaWServer. Прежний JSON v1 заменён; совместимость с ним не поддерживается.

## Кодирование

- Структурированные HTTP-запросы и ответы: `Content-Type: application/cbor`. WebSocket `/ws`: только бинарные кадры CBOR, без обязательного subprotocol.
- Все ключи map — UTF-8 имена полей непосредственно в CBOR. Реестров и сгенерированных ID нет. Новые валидные имена не требуют регистрации. [Обязательные требования](../../../docs/readme_cbor_requirements_ru.md).
- Числа и bool — нативные CBOR-типы; uint64-счётчики без десятичных строк. Браузер хранит большие целые как bigint. Значение π сохраняет double. Текстовые значения (имена, сообщения, enum) — CBOR text string; бинарный payload — byte string.
- В документации ниже приведены понятные имена полей. Например, `{v:2,action:"stop"}` означает CBOR map со строковыми именами этих полей, а не JSON-текст.
- Тело команды и целый WS-кадр <=4096 байт, включая CBOR-конверт. Декодер устройства: map <=16 полей, имя <=64 UTF-8 байт, array <=32 элементов, глубина <=4. Frontend/.NET при чтении: <=65536 байт, map <=128 полей, array <=256 элементов, глубина <=12. Indefinite containers, дубли ключей, trailing items, NaN/Infinity и числовые map-ключи отклоняются. Неизвестные валидные строковые поля сохраняются декодером либо игнорируются обработчиком без регистрации. Другая версия команды отклоняется.
- Открытая тестовая HTTP Basic пара: `demo / showcase-only`. Авторизация публичного WS не реализована. Basic по HTTP не шифрует данные.

## HTTP

| Маршрут | Данные | Доступ |
|---|---|---|
| GET /health | ok, v, source | Публичный |
| GET /api/v2/info | v, source, boot_id, chip, firmware, capabilities, limits | Публичный |
| GET /api/v2/summary | Envelope<Summary> | Публичный |
| GET /api/v2/tasks?offset=0&limit=8&snapshot_id=… | Envelope<TaskPage>, не более 8 задач | Публичный |
| GET /api/v2/connections?offset=0&limit=8 | ConnectionPage | Basic |
| POST /api/v2/control | v, profile=balanced/quiet/performance; только демо-строка в RAM | Basic |
| GET /api/v2/experiments | ExperimentState | Публичный |
| POST /api/v2/experiments | Команда нагрузки | Basic |
| GET /api/v2/led | LedState | Публичный |
| POST /api/v2/led | v, enabled, period_ms, duty_pct | Basic |
| GET /api/v2/strip | StripState | Публичный |
| POST /api/v2/strip | v, enabled, mode, color, brightness_pct, period_ms | Basic |
| GET/POST /api/v2/echo | Явный учебный пример query/form и text/plain ответа | Публичный |
| GET /api/v2/stream | Учебный text/plain buffered response, не SSE | Публичный |
| GET /go | Redirect в интерфейс | Публичный |

Ошибки приложения: CBOR `{v:2,error:{code,message?}}`; 400 неверные поля/CBOR, 401 Basic challenge, 403 чужой Origin, 409 сменившийся task snapshot, 413 body ceiling, 415 неверный MIME, 404 неизвестный маршрут. Низкоуровневые ошибки HTTP-сервера могут отличаться. Повторное чтение страниц задач при 409 должно начинаться с первой страницы.

Envelope: v/source/boot_id/seq/captured_ms/data. Summary: uptime_ms/sample_interval_ms/cpu_busy_pct/core_busy_pct/memory/rssi_dbm/task_count/ws_clients/network_status/counters/experiment/led/strip. Пулы памяти: id/label/total_bytes/free_bytes/min_free_bytes/largest_block_bytes. Counters: http_requests/ws_rx_frames/ws_tx_frames/ws_rx_bytes/ws_tx_bytes/rejected.

TaskPage: snapshot_id/offset/next_offset/total/items. Задача: id/name/state/priority/core_affinity/stack_min_free_bytes/runtime_ticks/cpu_pct. CPU доли вместе с IDLE нормализованы на устройство до 100%; runtime_ticks не подписываются как миллисекунды. ID сохраняются при сортировке. Смена boot_id сбрасывает историю UI; потеря связи помечает показания устаревшими.

## Нагрузка

Start: v=2, action=start, kind=pi/pulse, duration_ms=1000..30000 (по умолчанию 5000), on_ms=1..period_ms (200), period_ms=20..5000 (1000), core=доступное ядро (0), priority=1..5 (2). Stop: v=2, action=stop. Одновременно одна нагрузочная задача, завершение по длительности или команде.

ExperimentState: running/kind/duration_ms/on_ms/period_ms/core/priority/elapsed_ms/progress_pct/pi/iterations/active/sample_ms. π уточняется пакетами по 4096 итераций ряда Лейбница раз в 250 мс модельного времени. Это демонстрация промежуточного результата, не тест точности математической библиотеки. Pulse активен первые on_ms каждого периода. Рекомендуемый видимый пример: 200/1000 мс на 10 секунд.

Эмулятор вычисляет небольшое приближение π, а загрузку CPU моделирует: тяжёлую задачу на хосте не запускает. Tick модели 50 мс реального времени; WS-сводки 100 мс при нагрузке/анимации RGB, иначе 1 с. Скорость и pause относятся к модельному времени; частота прихода WS ограничена реальным временем. CPU-график эксперимента показывает последние 10 с, общий график — 60 с, буфер до 600 точек. Пропуски/aliasing возможны при быстрых режимах и замедленном браузере.

## Светодиоды

LedState: supported/enabled/period_ms/duty_pct/gpio/active_low/level. Обычный LED прежнего профиля SuperMini: GPIO8, active LOW; период 20..5000 мс, доля включения 0..100%. По умолчанию 1000 мс / 50%, т.е. 500 мс включён / 500 мс выключен. 0% постоянно выключен, 100% постоянно включён, enabled=false выключает. Анимированный предпросмотр отображает редактируемые настройки; текст «Выборка» относится к полученному состоянию.

Владелец подключил другую C3 и подтвердил WS2812/NeoPixel: 10 RGB, DIN на GPIO2. Наличие обычного LED на GPIO8 у новой платы не подтверждено; эмулятор сохраняет его как отдельный виртуальный опыт. До аппаратной реализации уточнить конфигурацию платы.

StripState: supported/enabled/gpio=2/count=10/mode/color/brightness_pct/period_ms/pixels. color — целое 0xRRGGBB (0..16777215), brightness_pct=0..100, period_ms=20..5000. Режимы: solid — одинаковый цвет; chase — один пиксель на шаг period_ms, 10 шагов за оборот; rainbow — полный цветовой цикл за period_ms. pixels — 10 целых RGB после применения яркости. По умолчанию цепочка выключена, яркость 20%. Выключение/нулевая яркость даёт десять нулей. UI отображает выборки; при периоде менее 100 мс некоторые фазы не видны. Физический драйвер, порядок байтов GRB и электрический сигнал пока не проверены.

## WebSocket

Телеметрия: `{v:2,type:"summary",source,boot_id,seq,captured_ms,data:Summary}`.

Echo: `{v:2,type:"echo.request",request_id,data:{text}}` либо `data:{bytes}`. Ровно один вид payload; request_id <=32 символов. Ответ type=echo.response с тем же request_id и payload. Ошибка CBOR/конверта — type=error, error.code=invalid_message. Текстовый WS-кадр и oversized frame закрывают соединение. Предел 4096 включает обёртку; тест byte string 3800 байт укладывается, 4096 байт payload с обёрткой уже может не уложиться.

В .NET максимум 4 WS-сессии, очередь 4 кадра на клиента; переполнение/ошибка отправки закрывает сессию. Это поведение эмулятора, не подтверждение новых библиотечных transport limits.

## Только эмулятор

GET /sim/api/v2/state: profile/scenario/paused/speed/seed/time_ms/boot_id/network_offline/sequence. GET /sim/api/v2/events?after=id: до 200 событий id/captured_ms/level/category/message.

POST /sim/api/v2/command: v=2/action/value?/number?. pause/run/step/reset; profile c3/s3; scenario normal/cpu/leak/stack/weak_wifi/traffic; speed 0.25/1/2/5; seed 0..99999; network — 8 реальных секунд недоступности /api, WS и SSE; disconnect — обрыв WS. Step только на pause, ровно +1000 мс. Сценарии памяти и трафика синтетические; сетевой шторм или реальная утечка не запускаются. Модель S3 — только демонстрационный профиль, не проверенная поддержка firmware.

Host привязан к loopback и проверяет RemoteIpAddress/Host/Origin. /sim остаётся доступен при имитации потери связи. SimulatorPanel и его CSS/команды загружаются отдельным chunk только по capability simulator_control.supported. `device-manifest.json` и firmware assets.hpp исключают эти ресурсы; общий UI, эксперименты и управление светодиодами остаются одинаковыми.

Реализация: Showcase.Contracts/Dtos.cs, CborCodec.cs, корневой frontend/src/contracts. Проверки: Showcase.Tests и frontend/tests/ui-smoke.mjs. Это локальные проверки, без аппаратного подтверждения.


## Реализация C3 adapter

Подготовлена в main, см. [передачу на аппаратную проверку](readme_device_ru.md). Те же строковые ключи, Vue assets и CBOR v2 endpoints; source=device, simulator_control=false. Реальные CPU/heap/FreeRTOS task snapshots, π/pulse worker и WS2812 RMT GPIO2/10/GRB. GPIO8 disabled, LED capability=false; UI скрывает его панель. Нет /sim API.

Прикладной firmware parser ограничен map <=16 полей и одним WS data map; команды не требуют произвольных вложенных структур. Защищённые маршруты используют статические Route + makeChain(AuthMiddleware,AccessMiddleware). request.body() разбирается в callback, в worker остаются только typed values. Указатели на body не сохраняются. 401/403 из core могут быть text/plain; это учтено UI.

Детали WS-соединений пока не предоставляются: connections capability=false, endpoint возвращает 501. TX счётчики считают попытки очереди, не подтверждённую доставку; новые transport/admission limits не подменяются прикладным кодом. max_ws_clients=4 пока целевой предел. Источник/частота CPU и ограничения timing описаны в readme_device_ru.md. Аппаратный smoke основного чата прошёл; границы результата и оставшиеся проверки приведены в readme_verification_ru.md.


## SSE и read-only проверки доступа (19.10.2026)

| Маршрут | Формат / назначение | Доступ |
|---|---|---|
| GET /api/v2/events | text/event-stream, diagnostic раз в секунду | Public |
| GET /api/v2/events/private | Тот же diagnostic stream | Basic, права 1 |
| GET /api/v2/auth/status | text/plain при успешном доступе | Basic, права 1 |
| GET /api/v2/auth/admin | Fixture имеет права 1, поэтому 403 | Basic, права 2 |

`event: diagnostic`, `id: BOOT:SEQ`, `retry: 1500`; `data:` содержит JSON:
`{v:2,source,boot_id,seq,captured_ms,cpu_busy_pct,free_bytes,task_count}`.
Это отдельный текстовый формат SSE, без CBOR/base64 и без изменения строковых ключей
основного wire-контракта. Последние 80 событий и 120 точек графика держит браузер.
Last-Event-ID не даёт replay: история на сервере не хранится. Поток не является ACK.
SSE и WS можно независимо отключать; summary не используется для наполнения SSE-графика.
Отказ авторизации приходит до начала SSE-ответа. `401` содержит Basic challenge;
фикстура `demo:showcase-only` имеет права 1; запрос прав 2 возвращает 403.
Ошибки core Route могут быть text/plain; эмулятор использует CBOR ошибки.
Учебный `/api/v2/stream` остаётся одним buffered text/plain ответом, отдельным от SSE.

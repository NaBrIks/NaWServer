> Актуальная аппаратная проверка 19.10.2026: [WS/SSE, авторизация и настоящий UI на C3](readme_c3_realtime_verification_ru.md). Ниже сохранено состояние до прошивки.
> Актуальное обновление 19.10.2026: [новый UI, SSE, auth и результаты](readme_refresh_20261019_ru.md).
> Ниже сохранены результаты предыдущей версии; её аппаратные проверки не подтверждают новую прошивку.
# Showcase: подготовка и передача на аппаратную проверку C3

Статус: firmware showcase прошит и проверен основным чатом, без прошивки этим чатом. Основной чат владеет аппаратным запуском. Результаты приведены ниже; отчёты minimal не являются проверкой showcase.

## Конфигурация

Владелец подтвердил новую C3: 10 WS2812/NeoPixel, DIN на GPIO2. Используется espressif/led_strip 3.0.1, RMT, 10 MHz, GRB, без DMA. GPIO8 не трогается: обычный LED новой платы не подтверждён, capability led=false и его панель скрыта. Опциональный NAWSERVER_DEMO_LED_GPIO допускается только после отдельного подтверждения GPIO/active LOW; текущая поставка использует только WS2812.

При старте нагрузка выключена, цепочка выключена (передаются нули), выбран цвет 0xffa030 и яркость 20%. RMT init/refresh ошибка делает strip unsupported, вместо имитации успешного вывода.

Секреты: игнорируемый main/wifi_secret.hpp подготовлен основным чатом. Здесь его содержимое не читалось/не печаталось. Открытая Basic-пара демки: demo / showcase-only. Настройки только в RAM.

## Файлы и обязанности

| Файл | Назначение |
|---|---|
| main/main.cpp | Статическая таблица handlers/Route, Basic chain, HTTP/WS, gzip assets, Wi-Fi bootstrap |
| main/wire.hpp/.cpp | TinyCBOR: фиксированный output buffer, строковые имена полей, проверка bounded команд |
| main/runtime.hpp/.cpp | bench_worker, output worker, typed параметры, RMT/опциональный GPIO |
| main/telemetry.hpp/.cpp | Паспорт SDK/чипа/build/reset, heap, runtime stats задач, CBOR snapshots |
| main/assets.hpp | Генерация из общей production UI сборки |
| main/idf_component.yml | Прикладные pinned CBOR/RMT зависимости, ядро не меняется |

request.body() возвращает borrowed ByteView. CBOR разбирается в callback, затем остаются только числовые параметры/enum в защищённом состоянии workers. Указатели на тело/CborValue/request в задачу не передаются. Дополнительная копия тела не нужна; copyBodyTo остаётся API для сценариев, которым нужна собственная память.

Commands: definite map <=16 полей, только строковые имена полей и scalar values; для WS разрешён один вложенный data map. Текст проверяется на UTF-8. Parser принадлежит Fields, копирование Fields запрещено; дочерний Fields не переживает родителя. Общий предел 4096 байт. HTTP и WS receive callbacks сериализованы HTTPD и используют общий static output/echo buffers; producer имеет собственный buffer. Каждая отправка копируется текущим WS backend.

## Реальная нагрузка и диагностика

Один bench_worker на core0, priority 1..5, duration 1..30 s. π вычисляется реальным CPU, промежуточные значения публикуются раз в 250 ms. Pulse выполняет вычисления первые on_ms каждого period_ms (20..5000). В активном участке есть 1-tick yield примерно каждые 10 ms, чтобы IDLE/watchdog могли работать; это не обещание 100% CPU или точного генератора прямоугольного CPU-сигнала. HTTP не выполняет расчёт и не ждёт его окончания. Stop/поколение отменяет старую задачу, перезапуск ограничен одной активной нагрузкой.

CPU считывается по delta runtime IDLE раз в 100 ms; это измерение, а не вычисление из команды нагрузки. Подробный bounded снимок максимум 32 задач — 1 Hz. Имена копируются под приостановленным scheduler на однопроцессорной C3. Минимум стека в ESP-IDF C3 измеряется байтами (StackType_t=uint8_t). Native runtime counter учитывается с модульной разницей uint32 и накапливается в uint64; runtime_hz=1 MHz для выбранного ESP_TIMER. Не интерпретировать это как portable формат всех RTOS.

WS summary 10 Hz при нагрузке/анимации, иначе 1 Hz. Task snapshot стабилен между 1 Hz сборами, при смене snapshot_id возвращается 409; UI повторяет следующий опрос. Heap: internal 8-bit total/free/minimum/largest block. S3 hardware здесь не проверяется.

## Запуск основным чатом

Сборка, без upload:

```powershell
$env:PLATFORMIO_CORE_DIR='<repo>/local/pio-core'
pio run -d examples/showcase -e c3 -j 2
```

Platform espressif32@6.10.0 / ESP-IDF 5.4. Defaults включают WS, trace/runtime stats, ESP_TIMER, FreeRTOS 1000 Hz. Окружение c3 выбирается явно. Никакой auto-upload в Start-Showcase или тестах нет. Существующая partitions.csv сохранена; предупреждение littlefs/subtype 0x82 относится к прежней таблице, перед прошивкой её оценивает основной чат. Не стирать NVS/flash целиком.

После координированной прошивки и получения фактического IP открыть `http://IP/#experiments`. Для первого показа: включить RGB, solid, один цвет, яркость 10–20%, применить; потом выключить. Нагрузку пока не запускать автоматически.

Аппаратный smoke выполнен основным чатом без --rgb: 20 PASS. Этот чат его на плате не запускал. Команды для следующего согласованного прогона:

```powershell
cd examples/showcase/frontend
node tests/device-smoke.mjs --url http://DEVICE_IP
# Только по явному решению основного чата: красный 10% на 1 s, затем off:
node tests/device-smoke.mjs --url http://DEVICE_IP --rgb
```

Без --rgb проверка только читает HTTP/CBOR, задачи, SHA-256 общих ресурсов и WS summary/echo. Нет автозапуска нагрузки. Успешный ответ/RGB readback не подтверждает видимый свет: цвет/все 10 пикселей проверяет владелец отдельно.

## Известные ограничения

- Details списка WS-сессий недоступны в текущем core API: capability connections=false, защищённый endpoint 501. Summary count доступен.
- TX счётчики — попытки постановки кадров в существующий void send API, не подтверждение доставки. Core пока может отбросить кадр при переполнении, новые transport limits этим адаптером не реализуются. max_ws_clients=4 — согласованный целевой лимит, отдельный admission guard этой версией не гарантирован.
- 401/403, формируемые core Route, могут быть текстовыми; прикладные ошибки CBOR. Authenticated WS и SSE недоступны.
- Фиксированные stack frames не заменяют аппаратный watermark. Тайминги/нагрузка/RMT под Wi-Fi проверяются после прошивки отдельно.
- Сборка использует явный C3 target; board id devkitm-1 — конфигурация SDK, не утверждение об установленном LED на GPIO8.

## Готовый артефакт

Финальная C3 сборка SUCCESS: `.pio/build/c3/firmware.bin`, 982192 B, SHA-256 `1e645fac6a03694d1d7339265e5b0b74b53faca33dc74759d5dc0571e6ceddf3`. Flash981792 B, static RAM48064 B; device assets58753 B gzip. Копия firmware не публикуется и не коммитится. Первичная передача и аппаратный прогон завершены; freeze снят. Boot/flash/IP/protocol/hardware результаты различаются и перечислены ниже.

POST RGB подтверждает принятые настройки; `pixels` отражают последний успешный вывод output worker. Для проверки цветового readback после POST дать worker время (например 100–200 ms либо короткий bounded retry). Успешная передача RMT и readback не заменяют визуальное подтверждение цвета владельцем.

## Аппаратный результат основного чата — 18 октября 2026

Источник: сообщение основного чата по завершении его аппаратного прогона. Этот чат проверку не повторял и платой не управлял. Проверен именно showcase artifact SHA-256 `1e645fac6a03694d1d7339265e5b0b74b53faca33dc74759d5dc0571e6ceddf3`, а не прошивка minimal.

- Flash: PASS, выполнена основным чатом на согласованной C3. Разметка совпала с minimal; NVS не стиралась. Новая резервная копия не делалась по отдельному указанию владельца.
- Boot: PASS, `NaWServer_showcase 0.1.0-showcase`.
- IP acquisition: PASS. Адрес устройства, сетевые сведения и сырые serial/network logs остаются в игнорируемой локальной области основного чата.
- Go hardware protocol check: PASS. source=device / C3, strip supported GPIO2 / count10, 401 без Basic, solid red20% с readback `0x330000`, rainbow с более чем пятью цветами, off и summary после команд.
- `frontend/tests/device-smoke.mjs` без `--rgb`: 20 hardware checks PASS. Реальные страницы задач, SHA-256 всех трёх device assets, binary WS summary и echo с NUL.
- Edge/Playwright: живая страница `/#experiments` загружена, screenshot сохранён основным чатом локально. Это отдельное подтверждение загрузки device UI, не полный повтор 63 эмуляторных browser checks.

Нагрузка на плате не запускалась; π/pulse timing, CPU под нагрузкой, длительный прогон и stack/heap watermarks пока не проверены. Цепочка оставлена выключенной. Подтверждены RMT readiness, команды и readback; видимое свечение, правильность физического цвета и все десять пикселей должен подтвердить владелец. Аппаратный freeze снят основным чатом; последующие прошивки по-прежнему выполняет только он.

## Edge WebSocket: исправление лимита заголовков

Read-only проверка живой платы обнаружила отличие от Node smoke: штатный Edge получает `431 Request Header Fields Too Large` при каждом WS handshake; HTTP остаётся online. CDP показал около512 B только строк HTTP headers (User-Agent, Origin, extensions и другие стандартные заголовки). В исходной конфигурации `CONFIG_HTTPD_MAX_REQ_HDR_LEN=512`. Основной чат независимо подтвердил 431 по HTTPD serial diagnostics. Адрес и сырые headers/logs в этот документ не переносятся.

Контрольный Edge с коротким диагностическим User-Agent на той же прошивке подключился: HTTP online / WS online, 46 принятых кадров, socket errors0. Это подтверждает parser ceiling, а не ошибку CBOR или request.body(). Диагностический браузер не отправлял LED/нагрузочные команды; изменение User-Agent не предлагается как пользовательское решение.

По согласованию с основным чатом изменены только showcase sdkconfig.defaults и активный sdkconfig.c3: `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`. Сгенерированный config/sdkconfig.h подтверждает1024; URI остаётся512, body/frame4096. Core, firmware sources и Vue assets не меняются. Требуется пересборка/обновление основным чатом и повтор WS в обычном Edge; до этого стандартный browser WS не считается прошедшим аппаратную проверку.

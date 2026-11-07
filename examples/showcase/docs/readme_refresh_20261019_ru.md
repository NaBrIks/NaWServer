> Актуальная аппаратная проверка 19.10.2026: [WS/SSE, авторизация и настоящий UI на C3](readme_c3_realtime_verification_ru.md). Ниже сохранено состояние до прошивки.
# Обновление Showcase — 19 октября 2026

## Реализация

Проверена интеграция с текущими Request/Response, response-only endpoints,
WebSocketEndpoint/WebSocketClient и библиотечным `sse::EventSource`.
Core и исходный TPMS-проект этой задачей не менялись. Ранее находившиеся в checkout
изменения API сохранены; исторический API обратно не добавлялся.

Интерфейс адаптирован к UI.TechnicalShell: локальные Tabler Icons и MIT license,
четыре исходные палитры, три плотности, RU/EN, фиксированная оболочка, отдельные
прокрутки, компактные счётчики, схема HTTP/WS/SSE, доступные справочные диалоги.
SSE не подменяет WS: отдельный график RAM и журнал диагностики работают параллельно
с CBOR-телеметрией и echo. Старый UI ошибочно называл SSE отсутствующим.

На C3 EventSource создаётся статически, запускается после SDK init и использует
свой bounded worker. Подписчики представлены только Token; таблица защищена
краткой critical section, публикация копирует Frame и не ждёт сеть. Публикуется
снимок раз в секунду. Request/Response/Next не сохраняются. Перед будущим shutdown
надо остановить producer, успешно drain/stop EventSource, затем server.end().
.NET использует асинхронные SSE-ответы: максимум 4 подписки, один снимок/секунду,
deadline записи 5 секунд, без неограниченной очереди и replay. Общий лимит TCP
ядра не увеличен: четыре SSE-слота не обещают четыре браузера с WS/HTTP одновременно.

HTTP Basic уже имелся для команд. Добавлены отдельные read-only probe endpoints
и экран с явными 401 (без данных/неверный пароль), 200 и 403 (права 1 против 2).
Защищённый SSE открывается только после авторизации; native EventSource остаётся
на публичном потоке, защищённый поток проверяется fetch со streaming reader и
явным Authorization. Учебные credentials не сохраняются в localStorage.
Authenticated WS по-прежнему неподдерживаемый: HTTP pipeline его не защищает.

При совместном приходе HTTP и WS старый summary с меньшим seq теперь не
перезаписывает более свежий снимок того же boot.

## Проверки этого запуска

| Проверка | Результат |
|---|---|
| Vue type-check / production Vite build | PASS |
| .NET solution, stable SDK 10.0.402 | PASS, 0 warnings / 0 errors |
| Showcase.Tests, модель + live HTTP/CBOR/WS/SSE/auth/assets | 102 PASS |
| TinyCBOR C++ / .NET interop | 53 C++ + 2 обратные .NET PASS |
| CUA browser acceptance, текущая production сборка | 20 PASS |
| Showcase / ESP32-C3 / ESP-IDF 5.4.0 | build PASS |

Браузерный harness выполнен именно через `runShowcaseSmoke(tab.playwright)` в CUA:
Cream/Nano, все видимые Tabler имена, диалог/Escape/focus restore, SSE при
отключённом WS, WS echo при отключённом SSE, Unicode query/form, buffered response,
401/401/200/403/401, защищённое SSE событие, защищённый список WS, задачи/фильтр,
π progress, RGB command/readback, журнал SSE, разные владельцы прокрутки, Dark/EN.
Реальный viewport этого прогона 857×1006 CSS px; телефонные размеры и полноценный desktop
1440+ в этом запуске не измерялись. Снимки и JSON — ignored `local/screenshots/`.

Assets: 97 701 bytes gzip всего; 94 551 device; 3 150 simulator-only.
В прошивку не включён lazy chunk пульта; локальный font и его лицензия включены.
Сверены SHA-256 обычных и gzip HTTP-ресурсов с manifest.

Firmware: 581 936 bytes; static RAM 47 000 bytes; flash sections 581 544 bytes.
SHA-256 firmware.bin:
`862edb1835743c480e1817c42b296ebcbae61a0207484ed21e6bd9feff204997`.
Собственный compiler stack frame app_main — 1408 bytes, не полная глубина стека
и не runtime watermark. Первый incremental link использовал старый CMake cache без
SSE objects; штатный PlatformIO clean и полная сборка решили это без правки ядра.

Новая прошивка этой задачей не записывалась. Аппаратные SSE+WS, RGB-свечение,
stack watermark и длительная нагрузка не подтверждены этим отчётом. Старые
аппаратные результаты относятся к прежней прошивке и не распространяются на эту.

Локальный Host оставлен на http://127.0.0.1:8788 и зарегистрирован в локальный монитор стенда.
Коммит/push не выполнялись.

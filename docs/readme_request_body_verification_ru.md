# Проверка body API на ESP32-C3, 18 октября 2026

Контракт: [raw и явное копирование](readme_request_body_ru.md).

- 30 проверок RequestBody PASS: binary/NUL, exact/overflow/null/empty, неизменность
  назначения при отказе, независимость копии, move и capacity std::vector.
- Go unit tests PASS; smoke собран из стандартной библиотеки Go.
- Minimal / C3 PASS: ESP-IDF 5.4.0, GCC 14.2.0, C++17, espressif32@6.10.0, -j 2.
  Статический RAM 29312, flash 867418 байт; это linker sizes, не heap peak.
- Другие ESP32 targets по текущему указанию владельца не собирались.

Прежняя COM5 исчезла; на COM8 идентифицирована другая ESP32-C3 rev0.4. Владелец
подтвердил замену тестовой платы и разрешил её проверку. По последующему указанию
не делать сохранений резервное чтение остановлено; новая полная копия не создана.
Прошивка minimal PASS с проверкой хэша, boot PASS, IP получен, HTTP server ready.
Отдельных команд стирания NVS не было. Конкретные MAC/IP и serial logs остаются
за пределами отслеживаемой документации. Основной TPMS не переключался.

Встроенные loopback-проверки: 13 PASS, 0 FAIL, free_heap в конце 248044 байта.
Это разовый снимок после тестов, не измерение стабильности под нагрузкой.

Первый сетевой прогон завершился timeout на /health. Повтор при чтении консоли прошёл,
затем полный повтор с закрытой USB-консолью также PASS. Причина начального timeout
не установлена; зависимость сервера от открытого serial monitor не воспроизведена.

- HTTP health/404, query/form, Basic rejection/success, DELETE — PASS.
- Частичный POST, предел тела 4096/4097 и последующий health — PASS.
- Body API: 28 HTTP checks, raw/pointer/C-array/std::array/vector, пустое тело,
  NUL/не-текстовые байты, размеры 64/65/4096, точный binary echo/MIME, отказ
  фиксированного буфера и запрет без авторизации — PASS.
- WS text/binary 0..4096, пять reconnect и последующий HTTP — PASS.

Логи в игнорируемом local/: test-request-body.log, build-body-api-c3.log,
flash-body-api-com8.log, body-api-smoke-output.log, test-body-api-c3-console-closed.log.
Плата оставлена с minimal и защищённым /private/body; demo-chat получил контракт
и результаты. examples/showcase здесь не изменялся.

Это короткие последовательные проверки одной C3. Stop/drain, slow clients, soak,
streaming, auth-before-body, SSE, защищённый WS и другие SoC не проверены этим этапом.

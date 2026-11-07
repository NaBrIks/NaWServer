# Smoke-клиент для examples/minimal

Стандартная библиотека Go, без сторонних зависимостей. Это короткие последовательные
сетевые проверки, не нагрузка и не симуляция firmware. Запускать только против
экспериментальной платы с `examples/minimal`; для showcase маршруты/fixtures другие.

Из корня репозитория, подставив адрес платы вместо документационного адреса:

```powershell
go -C tools/smoke run -buildvcs=false . 192.0.2.1
```

Поддерживаются `host`, `host:port` и `http://host:port`, включая IPv6 в скобках.
Реальные Wi-Fi credentials не читаются и не используются. HTTP Basic использует
исключительно открытый fixture minimal: `test` / `test-only`.

Проверяются HTTP health/404, query/form decode, Basic success и malformed/empty/
truncated/wrong denial, DELETE, POST частями, отказ на body 4097 байт, публичный WS
text/binary 0/1/127/128/256/4096, пять reconnect и HTTP после WS. Reset/EOF на oversized
body допускается только с последующим успешным health; timeout не считается отказом
сервера. Время ожидания каждой сетевой операции ограничено. Вывод не печатает заголовки
Authorization и содержимое закрытых ответов.

WS codec реализует только нужные smoke-проверкам complete frames, не универсальный
WebSocket client. Fragmentation, SSE, TLS, authenticated WS, slow-reader нагрузка,
stop/restart и heap не проверяются. Запись flash/NVS и изменение настроек отсутствуют.

Локальная проверка самого инструмента:

```powershell
go -C tools/smoke test ./...
```

Она проверяет границы frame lengths/masking, отказ на некорректные/слишком большие
кадры, частичные записи и разбор адреса. Успех этих тестов не означает проверки платы.
Новый инструмент заменяет историческую Python-версию smoke, не меняя vendor tools
ESP-IDF/PlatformIO.

Актуальный minimal также проверяется через POST /private/body: raw и copyBodyTo для
своей памяти, C-array, std::array, vector; точные binary/NUL байты 0..4096, переполнение
фиксированного назначения и запрет без Basic. Требуется свежая прошивка с этим маршрутом.

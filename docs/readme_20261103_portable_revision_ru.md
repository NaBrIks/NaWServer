# Ревизия общего HTTP engine:03.11.2026

## Реализованный контракт

Native ESP-IDF HTTPD удалён. Все платформы используют HttpServer/TcpHttpBackend
и общие HTTP/WS/SSE; платформенные sockets, worker и allocators сохранены.
Старые build selectors отвергаются с сообщением миграции, не работают как aliases.

`HandlerBase::routes()` возвращает декларативные RouteDescriptor. `canHandle`
удалён. Выбор не читает тело: exact раньше prefix, более длинный prefix раньше
короткого, несовпадение метода даёт405/Allow. Новые API не сохраняют совместимость
с неопубликованными старыми обработчиками. Потребители репозитория перенесены.

Ранняя HeaderChain использует общий исполнитель middleware. HeaderRequest не
предоставляет body API. HeaderResponse допускает небольшой ответ либо Drop, не
потоковую отдачу. Авторизация и AdmissionMiddleware работают до body/upgrade.
Квоты не входят в TCP accept; HttpConnectionManager знает только физические
места и непрозрачное разрешение, принадлежащее запросу/соединению.

`Result::Drop` и `Response::drop()` терминальны: middleware не может превратить
их в обычный fallback ответ. Ожидающий TX отменяется, native close выполняет
владелец I/O. Обычное закрытие после ответа сохраняет доставку очереди в транспорт;
оно не означает подтверждения peer. Keep-alive сохранён.

## Приём больших тел

Маршрут задаёт BodyOptions:

- `Buffered`: небольшой известный Content-Length, предел до выделения RAM;
- `None`: тело не допускается;
- `Stream`: конечный max_bytes, block_bytes и begin factory получателя.

BodyReceiver владеет контекстом через move-only callbacks. `consume(ByteView)`
возвращает Accepted, Pause либо Error. Accepted принимает весь блок; Pause не
принимает ничего, тот же блок предлагается повторно. View нельзя сохранять после
callback. Finish вызывается после полного тела, cancel при обрыве/ошибке, release
единожды. Файлы/SD/OTA реализуют разных получателей поверх этого общего контракта.

Управляемый transport читает не больше настроенного блока и уступает другим
соединениям при ожидании. Прикладной consume/read callback сам обязан быть коротким;
блокирующая операция FS или длительное вычисление всё ещё задерживает dispatcher.
Физический `managed.request_body_block_bytes` и `NAWSERVER_TCP_MANAGED_BODY_BLOCK_BYTES`
ограничивают размер поддерживаемого блока. Режим требует managed engine.
Входящий chunked transfer и Expect:100-continue сейчас не поддерживаются.

## Отдача файлов

`sendOwnedSource(status, type, OwnedResponseSource)` передаёт владение bounded
источником с известным размером. В managed engine callback завершается, а чтение
и отправка продолжаются порциями между другими соединениями. Release закрывает
ресурс по завершении/отмене. В RP-демо используется ограниченный пул file readers.
Время жизни FS mount и внешнего owner всё равно обеспечивает приложение.

Старый borrowed `sendSource` и последовательный writer остаются синхронными:
заимствованный источник нельзя использовать после возврата. Нельзя считать их
эквивалентом owned source по задержкам. `sendOwned` передаёт готовый memory lease;
`sendImmutable` требует неизменяемое содержимое с достаточным временем жизни.

## Сборка и границы

`NAWSERVER_AUTH`, `NAWSERVER_CLIENT_QUOTAS`, `NAWSERVER_TCP_MANAGED` независимы.
Сборочная фабрика принимает MANAGED, NO_AUTH, NO_CLIENT_QUOTAS, NO_WEBSOCKET, NO_SSE.
Примеры с закрытыми маршрутами по умолчанию сохраняют аутентификацию. Отключение
AUTH не должно молча превращать такие маршруты в публичные.

Реализованы bounded identity resolver, несколько защищённых профилей, статистика
policy и перенос разрешений HTTP→WS/SSE. Дополнение: генерация/ротация ключей через NVM/web/CLI и метки страниц реализованы следующим этапом; см. [руководство ключей](readme_api_keys_ru.md). TLS и OTA/log модули остаются отдельной работой.
Общего deferred completion API для произвольных прикладных вычислений нет.

## Проверки

- Windows/MSVC Release:61 активный тест PASS;3 прежние STL-allocation проверки
  явно disabled на MSVC, не включены в число прошедших.
- Linux/Zig0.14.1 через WSL:64/64 PASS, включая эти allocation проверки.
- MCP bounded/yyjson/RapidJSON:по4/4 проверки на каждый reader.
-32 комбинации реестра источников;4 реальные независимые сборки AUTH/QUOTAS/
  MANAGED/protocol-off. AUTH0 проверяет отсутствие auth-полей при компиляции.
- Новые lifecycle tests: upload512КиБ блоками<=128байт с Pause и соседним
  быстрым запросом; owned download512КиБ; keep-alive; неполное тело/cancel/release;
  Drop при Filling/Writing и повторном native handle; WS/SSE permits в managed
  и unmanaged, освобождение на том же HTTP-владельце.
- Firmware builds: RP; C3 minimal/showcase/compile_check/SSE/SSE-device/
  SSE-integration; C6managed; S3managed. IDF5.5.6, Pico SDK2.3.1, FreeRTOS11.3.3.
- Аппаратно загружены только приложения C3/Pico, разделы/NVS сохранены. C3
  собирался с CONFIG_COMPILER_OPTIMIZATION_PERF, RP с CMAKE_BUILD_TYPE=Release.
- C3:2 различные тестовые идентичности/2WS+SSE,10 загрузок JS226275байт,
  одинаковый SHA-256, оба WS получили47 сообщений и22pong. Отдельные запросы
  защищённого ключа проходят одновременно. Загрузка ресурса225–456мс.
- Pico:2WS+SSE,8 загрузок JS229641байт,106 сообщений и53echo на каждый WS,
  11SSE событий; максимальный интервал WS124мс, ресурсы921–982мс.

Pico первоначально возвращал CYW43_LINK_NONET, затем JOIN/no-IP; подключился
и получил адрес без изменения credentials/FS. Это отдельно от запуска сервера.
Измерения выше — короткая функциональная нагрузка, не долговременная приёмка
stress, не сравнение CPU со старой версией и не доказательство устранения всех
ранее замеченных потерь памяти. TX rejection counters включают backpressure/
повторные попытки и сами по себе не равны потере HTTP payload.

S3 MAIN firmware, NVS и общая FS не изменялись: выполнена только сборка.
Две существовавшие до работы локальные правки сохранены вне implementation
commit: readme_tcp_http_ru.md и независимые изменения integration test.

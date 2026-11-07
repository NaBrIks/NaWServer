# Проверка MIME и результатов WS-отправки — 19 октября 2026

Проверяемая правка: MIME в static handlers, явные результаты WS send/broadcast,
очистка неиспользуемых includes и alias String. Регистрация маршрутов и runtime
add/remove не менялись. Контракты и действия приложения: [guide](readme_static_ws_ru.md).

## Host

Локальный Zig C++17, конфигурация CMake `local/structure-zig`, warning-as-error.
Сборка и CTest: **9/9 PASS**. Новый `transport_test`: **42 проверки PASS**.

Тест компилирует настоящие static handlers, Request/Response и WS sender. SDK
заменён небольшим host stub; payload allocator и nothrow allocation подменяются
только внутри теста, без test hooks в firmware.

Проверено:

- text/html по умолчанию, явный text/css, binary application/octet-stream,
  явный text/javascript вместе с gzip; payload сохраняет NUL и 0xff;
- nullptr, пустой и CR/LF MIME: отказ до transport send;
- InvalidArgument, TooLarge, NoMemory для задания и payload, QueueError;
  после отказов pending снова 0;
- четыре задания принимаются, пятое даёт QueueFull; после исполнения pending 0;
- копирование данных до возврата, пустой binary, Closed;
- empty broadcast, частичный broadcast 1 queued / 1 rejected с QueueFull;
- disconnect до исполнения pending: старые задания не передают данные.

Тесты не подтверждают поведение реальных FreeRTOS, HTTPD, TCP buffers, повторного
использования fd и остановки сервера под нагрузкой. SDK stub выполняет задания
детерминированно и не моделирует эти компоненты.

## ESP32-C3

Попытка `pio run -d examples/minimal -e c3 -j 2` с project-local
`PLATFORMIO_CORE_DIR=local/pio-core` остановилась до компиляции:
`MissingPackageManifestError: Could not find one of 'package.json' manifest files in the package`.
Локальное окружение неполно; манифест framework-espidf отсутствует. Глобальные
инструменты и пакеты не менялись. **Успешная C3-сборка этой правки не подтверждена.**

Flash, boot, IP acquisition и сетевые проверки этой ревизии не выполнялись.
Аппаратные результаты прежних ревизий к ней автоматически не относятся.

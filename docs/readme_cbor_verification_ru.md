# Проверка прямых имён CBOR — 20.10.2026

Реализовано [решение владельца](readme_cbor_requirements_ru.md): UTF-8 имена
полей находятся в самих CBOR map. Оба JSON-реестра и C++ заголовка удалены,
удалены Vite alias и embedded resource .NET. Общий frontend, эмулятор,
C3 и S3 используют одинаковый формат. Числового fallback нет.
Новые валидные имена не требуют регистрации. HTTP/WS ядро библиотеки
по-прежнему работает с байтами и не навязывает приложению этот кодек.

## Автоматические проверки

- `npm test`: 9/9 PASS — прямые ключи на wire, произвольные вложенные UTF-8
  имена, uint64, double, binary/NUL, отклонение числовых ключей, дубликатов,
  некорректного UTF-8, trailing/indefinite данных и лимиты кодека;
  также сохранены проверки единственного frontend и изоляции LCD-расширения.
- `npm run build`: vue-tsc + Vite обеих целей PASS; ресурсы заново упакованы
  Go/.NET инструментами в firmware assets.
- Showcase.Tests: 108 PASS вместе с живым .NET Host на loopback, включая
  API, auth, CBOR, WS, SSE, модели и статические ресурсы.
- Native TinyCBOR ↔ .NET: 88 C++ проверок PASS плюс обратный обмен; проверены фактические text keys, неизвестные
  вложенные поля без реестра, uint64/double/bytes с NUL, все усечения WS-команды,
  отказы при переполнении writer и границы полей/массивов/имён/глубины.
- `pio run -e c3 -j 8`: SUCCESS; RAM 61848 B, flash sections 1077248 B.
- `pio run -e s3 -j 4`: SUCCESS; RAM 72688 B, flash sections 1347956 B.

На Windows GCC 14.2.0 стабильно падал в RGB-драйвере ESP-IDF при `-Og`,
в том числе с одним compile job. В CMake S3 добавлен условный `-Os` только
для компонента esp_lcd; версия SDK и его исходники сохранены. Обход и
исходная ошибка описаны в [issue Espressif](https://github.com/espressif/esp-idf/issues/12180).

## Физическая S3

Повторно идентифицированы ESP32-S3 rev 0.2 и MAC DEVICE-MAC на COM12.
Свежая таблица разделов сопоставлена с built partitions.bin. Записан только
factory app: 1348352 B, диапазон данных `0x10000..0x159300` (конец исключён),
в пределах раздела размером `0x500000`. Esptool подтвердил hash после записи.
SHA-256 образа: `f06df96658b2f5f6fd6012b369f888e926d3bc99eb6453efb11c2e55d2bbdd27`.
Bootloader, таблица и NVS не записывались. C3 не прошивалась.

`node frontend/tests/device-cbor-smoke.mjs --url http://192.0.2.2
--model ESP32-S3 --manifest examples/touch_lcd7/frontend/dist/device-manifest.json`:
27 PASS. Проверены:

- raw map keys всех уровней ответа: только строки; info 1113 B,
  summary 1372–1375 B, tasks (8) 1127–1132 B, network 227 B — меньше 4096 B;
- новые вложенные UTF-8 поля с uint64, double, array и bytes в echo-команде
  без изменений схемы; 3800 B бинарного payload с NUL возвращены точно;
- Unicode echo; ошибочные числовые ключи, дубликаты, UTF-8 и trailing item;
- binary WS summary и SSE diagnostic; LCD ready и живые FreeRTOS/heap данные;
- SHA-256 всех семи распакованных production assets совпадает с локальным manifest.

Первый WS-прогон не получил ответ и выявил недостаток timeout в проверяющем
скрипте. Скрипт исправлен: close и timeout теперь завершаются явной ошибкой.
Повторный полный прогон прошёл. Это smoke, длительная устойчивость сети не
измерялась; в браузере наблюдалось автоматическое восстановление HTTP.

В реальном in-app browser после reload проверены C3 emulator :8788 и S3 fixture
:8790: HTTP/WS/SSE онлайн, Unicode echo. На физической S3 :80 проверены те же
каналы, асинхронная LCD-страница с живыми данными и ответ echo. Скриншот и
локальные логи находятся в ignored `examples/touch_lcd7/local/`.
Ссылки трёх работающих интерфейсов обновлены через локальный монитор стенда helper.

Ограничение: новый wire несовместим с прежней числовой реализацией.
Frontend и firmware обновляются вместе; открытые старые вкладки нужно reload.

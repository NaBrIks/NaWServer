# Использование

Актуальный HTTP API: [Request/Response, общие заголовки и профили](readme_http_api_ru.md).
Датированные аппаратные проверки предыдущего API: [HTTP API verification](readme_http_api_verification_ru.md). Они не подтверждают текущую прошивку.

На ESP-IDF библиотека подключается через component CMake; для Linux, Windows и RP доступна [переносимая сборка](readme_portable_build_ru.md). Все платформы используют общий TCP HTTP/WS/SSE двигатель; нативный HTTPD удалён. `examples/minimal` — приложение с Wi-Fi,
`examples/compile_check` — проверка компиляции без зависимости от встроенного Wi-Fi.
Не выбирайте сетевой пример для SoC без собственного Wi-Fi без адаптации приложения.

В сетевом примере:

| Маршрут | Назначение |
|---|---|
| `/` | Текстовая страница |
| `/health` | JSON `{"ok":true}` |
| `/echo?value=...` | GET или form POST, возврат параметра `value` |
| `/private` | HTTP Basic, открытая тестовая пара `test` / `test-only` |
| `/ws` | Публичный WS echo, текст/бинарные кадры до 4096 байт; периодические `tick` |

Wi-Fi задаётся только в игнорируемом `examples/minimal/main/wifi_secret.hpp`. Пример
доступен рядом с ним; без файла приложение собирается, но сеть не запускает.
Для другой сети создайте файл копированием `wifi_secret.example.hpp`, если его
ещё нет, и заполните **локально в редакторе**:

```cpp
#pragma once
#define NAWSERVER_DEMO_WIFI_SSID "ИМЯ_РАБОЧЕЙ_СЕТИ"
#define NAWSERVER_DEMO_WIFI_PASSWORD "ПАРОЛЬ_РАБОЧЕЙ_СЕТИ"
```

Полный путь в текущем checkout:
`examples/minimal/main/wifi_secret.hpp`.
Не вводить реальные значения в tracked example header и не присылать их в чат.
`wifi_secret.hpp` исключён `.gitignore`. Это C++-строки: кавычка и обратный слеш
внутри значения записываются как `\"` и `\\`. После сохранения требуется **пересборка
и прошивка**: плата не читает header с компьютера при reset. Для default Lookup
собирается environment `c3`; переключение decoder описано в
[руководстве интеграции](readme_url_decode_integration_ru.md).

После загрузки ищите в serial строку `IP:` и `HTTP server ready`.
Роутер может требовать допуска MAC-адреса нового устройства.
Перед подключением к Wi-Fi диагностический пример выполняет встроенные проверки; длительность зависит от конфигурации. Wi-Fi power save отключён в примере для
проверки транспорта. После получения IP запустите с компьютера:

```powershell
# Замените документационный адрес адресом экспериментальной платы.
go -C tools/smoke run -buildvcs=false . 192.0.2.1
```

Инструмент использует стандартную библиотеку Go и открытые fixtures minimal;
подробности в [tools/smoke](../tools/smoke/README.md). Это короткий smoke, не нагрузка.
Собственная Python-версия заменена и удалена; vendor tools ESP-IDF не изменялись.

Для матрицы сборки: `pio run -d examples/compile_check -e c6 -e esp32 -e s2 -e s3 -j 2`.
Для неописанной PlatformIO-платы используйте установленный native ESP-IDF 5.4 и
`idf.py -C examples/compile_check -B build-<target> set-target <target> build`.
Это путь к проверке, а не уже полученный результат для всех целей.

Объекты обработчиков и их буферы должны жить дольше `server.end()`. Маршруты
регистрируются до `begin()`. Сервер останавливает задача приложения после прекращения
параллельных отправок, затем можно уничтожать обработчики. Остановка из HTTP callback
не поддерживается.

Ранняя HeaderPipeline выполняет auth и квоты до чтения тела и до WS/SSE upgrade. Прикладная Pipeline выполняется после получения тела. Маршруты объявляются через Route или таблицу RouteDescriptor; canHandle удалён. Защищать WS следует ранней политикой его обработчика, а не соседним HTTP URL. Общий сервер сохраняет ограниченный keep-alive и последовательные прикладные callbacks. HTTP Basic передаёт credentials без шифрования.
Новый API показан в [руководстве middleware](readme_middleware_ru.md); режимы Buffered/Stream для входа и owned source для больших файлов — в [HTTP API](readme_http_api_ru.md).

SSE runtime `src/sse` включён в компонент явно; публичный `sse.h` / `sse::EventSource` описан в
[руководстве SSE](readme_sse_ru.md). Его runtime запускается только через start().
Публичный API ещё не зафиксирован.

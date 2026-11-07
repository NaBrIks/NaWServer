# NaWServer Showcase — диспетчер ESP32

Единый Vue 3 / TypeScript frontend в корневой frontend/ используется C3, S3 и локальным ASP.NET Core эмулятором.
Текущая проверка и оформление от 19.10.2026: [отчёт](docs/readme_refresh_20261019_ru.md).

```powershell
cd examples/showcase
.\Start-Showcase.ps1 -Rebuild
```

Открыть [локальное демо](http://127.0.0.1:8788). После сборки работает без интернета:
локальные шрифты, Tabler Icons и ресурсы. Системные данные эмулятора явно помечены
синтетическими; HTTP, WS и SSE — настоящие соединения.
Для остановки только записанного процесса демо: `./Stop-Showcase.ps1`.

## Интерфейс

Применены палитры и правила TechnicalShell: Cream/Dark/Emerald/Light,
Nano/Mini/Norm, RU/EN, фиксированные шапка и меню, отдельная прокрутка рабочих
панелей. Настройки — последний пункт за разделителем. Пояснения открываются
кнопкой справки в модальном окне, Escape закрывает его и возвращает фокус.
Происхождение перенесённых файлов: [design/README](../../frontend/src/design/README.md).

- Обзор: схема обмена, компактные счётчики, CPU по WS/HTTP, память отдельно по SSE.
- Задачи: поиск, сортировка, runtime, минимум стека и история выбранной задачи.
- Память и соединения: ресурсы, счётчики и явные источники показателей.
- Лаборатория: query/form echo, binary CBOR echo, buffered response, redirect.
- Эксперименты: π и импульсы CPU, LED, WS2812 из 10 RGB на GPIO2.
- События / SSE: диагностические JSON-снимки раз в секунду, последние 80 событий.
- Доступ: проверки 401/401/200/403, WWW-Authenticate, защищённая SSE-подписка.

`WS /ws` передаёт CBOR-сводку и echo. `SSE /api/v2/events` — независимый
односторонний диагностический поток. Их можно отключать по отдельности.
`/api/v2/events/private` проверяет Basic до открытия потока. Открытый fixture:
`demo / showcase-only`, права 1; `/api/v2/auth/admin` требует права 2 и возвращает
403. Basic передаётся по HTTP, это учебная проверка. Authenticated WS в ядре
пока не поддерживается; интерфейс прямо показывает эту границу.

## Сборка и проверки

```powershell
npm.cmd ci --prefix ../..
npm.cmd run build --prefix frontend
dotnet build simulator/Showcase.sln
dotnet run --no-build --project simulator/Showcase.Assets -- frontend/dist
dotnet run --no-build --project simulator/Showcase.Tests
# При работающем локальном Host:
dotnet run --no-build --project simulator/Showcase.Tests -- --url http://127.0.0.1:8788
cd frontend
node tests/ui-smoke.mjs
```

`Showcase.Tests` — console harness, запускать через `dotnet run`, не `dotnet test`.
Live-проверки этого harness допускают только числовой loopback. Browser harness
также экспортирует `runShowcaseSmoke(page)` для CUA `tab.playwright`; standalone
режим использует установленный Edge и project-local Playwright.

В Visual Studio открыть `simulator/Showcase.sln`, запускаемый проект
`Showcase.Host`, профиль `Showcase`. `global.json` выбирает стабильный SDK .NET 10
с `latestPatch`; текущая проверка выполнена на 10.0.402. Host читает `frontend/dist`;
при изменении набора hashed assets после сборки нужно перезапустить только Host.

### C3: общий TCP-движок

ESP-IDF, Windows, Linux и RP используют общий HTTP/WS/SSE-движок. Нативный
ESP-IDF HTTPD удалён; переключателя backend больше нет. Полный Showcase требует
`CONFIG_NAWSERVER_WEBSOCKET=y` и `CONFIG_NAWSERVER_SSE=y`; отключение этих функций
предназначено для других приложений компонента.

Текущие PlatformIO-профили закрепляют Espressif32 8.0.0, ESP-IDF 5.5.6 и C++17.
Профили с суффиксом `idf6` используют отдельно установленный ESP-IDF 6.1.
Из корня репозитория:

```powershell
pio run -d examples/showcase -e c3
pio run -d examples/showcase -e c3_tcp
pio run -d examples/showcase -e c3_tcp_idf6
```

`c3` и `c3_tcp` сохраняют прежние имена каталогов сборки, но оба используют общий
движок. Их defaults/config/build независимы. Старый сохранённый sdkconfig имеет
приоритет над defaults: после обновления использовать чистый каталог сборки или
проверить `menuconfig`. Опции удалённого HTTPD не выбирают реализацию NaWServer.

Для обычного ESP-IDF shell сначала проверить `idf.py --version`, затем из
`examples/showcase`:

```powershell
idf.py -B ../../local/showcase-idf -DIDF_TARGET=esp32c3 -DSDKCONFIG=../../local/showcase-idf/sdkconfig -DSDKCONFIG_DEFAULTS=sdkconfig.tcp.defaults build
```

`GET /api/v2/info` сообщает фактические `firmware.idf_version` из app descriptor
и `firmware.http_backend` (`tcp`). Название build-каталога не подтверждает,
что соответствующая прошивка установлена на плате. HTTP остаётся незашифрованным;
TLS этой миграцией не добавлен.

Локальный Wi-Fi задаётся только ignored `main/wifi_secret.hpp`. Эти команды
собирают, но не прошивают устройство. Перед записью отдельно идентифицировать
разрешённую C3 и проверить диапазон app по текущей таблице разделов; не добавлять
backup или отдельные post-flash readback/hash passes. Исторические проверки
других прошивок не подтверждают новую комбинацию Showcase/SDK/backend.
[Аппаратные границы](docs/readme_device_ru.md).

Исторические `main.go`, `main_test.go`, `web/` используют старый контракт и не
являются текущим демо. Прикладной формат CBOR v2 описан в
[протоколе](docs/readme_protocol_ru.md); SSE имеет отдельный текстовый формат JSON.

Аппаратный отчёт: [реальные WS/SSE/auth/UI на C3](docs/readme_c3_realtime_verification_ru.md).

Управление RGB/LED: [кнопки режимов и немедленное применение](docs/readme_live_controls_ru.md).

RGB: [RMT / SPI без перепрошивки, запуск с RGB on / RMT](docs/readme_rgb_drivers_ru.md).

Текущее устройство интерфейса: [общая страница CPU/памяти/задач, фиксированные шкалы и уведомления](docs/readme_shell_resources_ru.md).

[Задачи в постоянной правой панели](docs/readme_task_dock_ru.md): изменение ширины, значки состояний, мини-графики каждого процесса.
Обязательный стандарт UI сохранён в [docs/ui-principles.md](docs/ui-principles.md).

Общая архитектура и профили: [frontend/README](../../frontend/README.md). Зависимости устанавливаются один раз в корне репозитория; локальный frontend/ содержит только профиль, расширения, команды и тесты.

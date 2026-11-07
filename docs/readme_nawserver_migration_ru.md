# Переименование в NaWServer

## Единый префикс настроек, 03.11.2026

Все собственные compiler macros и CMake options имеют uppercase `NAWSERVER_`;
ESP-IDF генерирует `CONFIG_NAWSERVER_`, демо-настройки — `NAWSERVER_DEMO_`.
Имена namespace, targets, include-путей и проектов от этого не меняются.
Текущие независимые флаги — `NAWSERVER_CLIENT_QUOTAS`, `NAWSERVER_AUTH` и `NAWSERVER_TCP_MANAGED`; для IDF добавляется `CONFIG_`. Старый `NAWSERVER_TCP_ADMISSION` удалён, физическое управление отделено от policy. Сокращённые имена не использовать как настройки.

| Старое имя | Новое имя |
| --- | --- |
| `NaWServer_MCP` | `NAWSERVER_MCP` |
| `NaWServer_JSON_*` | `NAWSERVER_JSON_*` |
| `NaWServer_MCP_OUTPUT_BYTES` | `NAWSERVER_MCP_OUTPUT_BYTES` |
| `NaWServer_BENCH_EXISTING_*` | `NAWSERVER_BENCH_EXISTING_*` |
| `SHOWCASE_LED_GPIO` | `NAWSERVER_DEMO_LED_GPIO` |
| `LCD7_WIFI_SSID/PASSWORD` | `NAWSERVER_DEMO_LCD7_WIFI_SSID/PASSWORD` |
| `NAWSERVER_WIFI_SSID/PASSWORD` | `NAWSERVER_DEMO_WIFI_SSID/PASSWORD` |
| `RP_DEMO_BOUNDED_WIFI_WAIT` | `NAWSERVER_DEMO_RP_BOUNDED_WIFI_WAIT` |

Compatibility aliases отсутствуют. Старые MCP/JSON настройки отклоняются
компилятором, старые CMake cache options — явной ошибкой конфигурации.
Для существующей сборки создать чистый build cache и передать прежние значения
под новыми именами. Переименовать также define в личных ignored wifi headers;
значения credentials не меняются. Исторические ADR/reviews/журнал сборок
сохраняют имена фактически проверявшихся тогда флагов.
Флаги SDK/ОС/компилятора (`PICO_*`, `CONFIG_FREERTOS_*`, `LWIP_*`, `YYJSON_*`)
сохраняют обязательные внешние имена.

Проверка переименования: Windows 59 активных тестов PASS (три ранее отключённых
не включались), Linux шесть целевых TCP/WS/SSE/stream тестов PASS. MCP checks
с новыми флагами — по 4/4 для bounded, yyjson и RapidJSON; configure также
проверяет отказ для неверных комбинаций и старого mixed-case compiler flag.
Старый CMake cache flag отклоняется, включая значение OFF. Standalone CMake
прошёл первоначальную и повторную конфигурацию и сборку MSVC; общий factory
теперь задаёт те же conforming C++17 switches, что host-цели.
RP/C3/S3 пересобраны; firmware/FS в этом этапе не записывались.

Проект переименован 23 октября 2026; 28 октября завершена очистка оставшихся
используемых идентификаторов. Исторические имена и результаты прежних сборок
остаются в истории Git, не являются доказательством проверок текущей версии.

Текущий публичный вход: `include/NaWServer`, `<NaWServer/...>`, namespace
`NaWServer`, CMake-компонент `NaWServer`, npm workspace `@nawserver/*`.
Имена ADR, планов и идей и ссылки на них обновлены. Проект SeMaps и папка его
модели называются `nawserver` и переименованы штатным редактором SeMaps.

## Изменения внешних идентификаторов 28 октября

- Basic Auth realm: `nawserver`.
- Ключи оформления браузера: `nawserver.*.appearance`; старые настройки в
  браузере не переносятся автоматически, настройки начнутся с умолчаний.
- Макросы файлов Wi-Fi: `NAWSERVER_DEMO_WIFI_SSID` и `NAWSERVER_DEMO_WIFI_PASSWORD`;
  локальные игнорируемые файлы этой копии обновлены без изменения значений.
- Vite для C6: `NAWSERVER_C6_URL`.
- Идентификаторы демо и дополнительного CBOR subprotocol используют `nawserver`.
  Если внешние потребители передают прежние идентификаторы явно, их нужно
  обновить; сторонние проекты в этой работе не менялись.

Старые CMake/PlatformIO-кеши могут содержать абсолютные пути и исходные имена;
при необходимости пересоздайте конфигурацию сборки. Переименование само по
себе не меняет лицензию и не завершает переход на MIT.

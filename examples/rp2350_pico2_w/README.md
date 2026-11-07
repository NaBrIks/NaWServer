# NaWServer: Raspberry Pi Pico 2 W demo

Нативная C/C++17 демо для **Pico 2 W, RP2350 ARM Cortex-M33, 4 МиБ Flash**.
Использует официальный Pico SDK, FreeRTOS, lwIP sockets и канонический общий
TCP HTTP/WS/SSE-движок NaWServer с общей очередью передачи.

Откройте `http://<DEVICE_IP>/`: IP своей платы указан в USB-журнале.

Демо использует общие lwIP/FreeRTOS адаптеры и CMake-цель библиотеки.
Настройка сети, LittleFS, NVS и аппаратная логика принадлежат этой папке.
См. [переносимую сборку](../../docs/readme_portable_build_ru.md),
[общую передачу](../../docs/readme_unified_transmission_ru.md) и исторические
наблюдения первого переноса в [compatibility.md](compatibility.md).

## Flash

Источник разметки: [partitions.json](partitions.json).

| Область | Начало | Конец включительно | Размер |
| --- | --- | --- | ---: |
| Прошивка | `0x000000` | `0x1FFFFF` | 2 МиБ |
| LittleFS | `0x200000` | `0x3BFFFF` | 1,75 МиБ |
| NVS / журнал настроек | `0x3C0000` | `0x3FFFFF` | 256 КиБ |

Прошивка имеет защиту от пересечения FS при линковке и проверку размера образа
при сборке. Обычная загрузка UF2 пишет только прошивку. FS и NVS не включаются
в UF2, не затираются и не заменяются при обновлении.

LittleFS монтируется приложением. Автоматическое создание разрешено только на
полностью пустой FS-области; повреждение/чужой формат не запускают форматирование.
Команда USB `f` явно инициализирует ещё не смонтированную FS и загружает только
именные web-ресурсы этой демо. Смонтированная FS не форматируется этой командой.

NVS здесь — отдельное **типизированное хранилище настроек RP**, а не бинарно
совместимая реализация ESP-IDF NVS. Настройки включают имя, Wi-Fi профиль и
счётчики. Используются два банка по 128 КиБ, записи по 512 байт, schema/generation,
CRC и отдельная commit-страница. Новый банк публикуется только после записи и
проверки данных; предыдущий остаётся исправной fallback-версией. Все операции
FS/NVS сериализованы одним Storage и проходят через `flash_safe_execute`.

## Среда и сборка

- Pico SDK **2.3.1**, Arm GNU **14.2.Rel1**, CMake/Ninja, picotool/pioasm **2.3.1**.
- FreeRTOS Kernel **V11.3.3**, официальный community RP2350 ARM NTZ port.
- Два активных CPU (FreeRTOS SMP), secure-only, MPU/TrustZone выключены;
  heap FreeRTOS 192 КиБ. Wi-Fi/HTTP/SSE/USB/FS/NVS закреплены за CPU0;
  вычислительную демо-задачу можно назначить CPU0 или CPU1.
- LittleFS **v2.11.3**.
- Общие установленные инструменты: `%USERPROFILE%\.pico-sdk`.
- Служебные SDK Python-скрипты выполняются на ПК; на плате интерпретатора нет.

Установка SDK/компилятора не выполняется этим проектом. Для большой загрузки
компилятора: [прямая ссылка Arm GNU 14.2.Rel1 для Windows](https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi.zip).
Официальный способ настройки среды описан в [документации Pico SDK](https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html).

PowerShell 7, из этой папки:

```powershell
.\Prepare.ps1
.\Configure-Wifi.ps1 -Profile YourSavedWiFiProfile
.\Build.ps1
.\Flash.ps1 -DeviceSerial '<USB_SERIAL>' -DeviceUrl 'http://<DEVICE_IP>'
.\Monitor.ps1 -DeviceSerial '<USB_SERIAL>' -Seconds 20
```

`Configure-Wifi.ps1` извлекает сохранённый профиль Windows в локальный игнорируемый
заголовок `.private/wifi_config.h`; пароль не выводится и не передаётся через
аргументы компилятора. Ранее сохранённый в NVS профиль сохраняется при прошивке.
`.deps`, `.private`, `build` и сырые логи исключены из Git.

Замените `<USB_SERIAL>` и `<DEVICE_IP>` идентификаторами своей проверенной платы.
`Flash.ps1` требует явный `-DeviceSerial` и использует команду
прошивки `load -x` **без `-v`**, отдельного readback и резервного копирования.
Сначала используется защищённая команда `/api/v2/control` по Wi-Fi, затем команда
приложения `b` и штатный vendor USB-reset. Адрес для обновления задаётся через
`Flash.ps1 -DeviceSerial '<USB_SERIAL>' -DeviceUrl 'http://<DEVICE_IP>'`; без URL
используются только USB-пути выбранной платы. Задача `usb-control`
работает отдельно от инициализации сети и HTTP. Команды: `b` — ROM BOOTSEL,
`r` — состояние, `f` — явная первоначальная подготовка FS.

## Демо

HTTP работает на стандартном порту **80**, адрес платы открывается без указания порта.
Выбранный сохранённый Wi-Fi профиль используется сначала с WPA2, затем с WPA3 при
неудачной попытке. Сеть должна быть доступна на 2,4 ГГц.

- `/` — общий Vue/TypeScript интерфейс ESP showcase: TechnicalShell, темы/плотность,
  графики CPU/RAM, панель задач, паспорт/сеть/MAC, лаборатория HTTP/WS, SSE и доступ.
  Компоненты, стили, шрифты и транспорт берутся из общего frontend.
  Профиль платформы задаёт RP-названия и страницу Flash/NVS. Общий индикатор
  сообщений открывает подробности по нажатию. `frontend/dist` генерируется;
  gzip-ресурсы обслуживаются из LittleFS, при недоступной FS — из Flash.
- `/health`, `/api/status`, `/api/storage` — состояние, heap, разметка и расход FS/NVS.
- `GET /api/settings`, `POST /api/settings?name=...` — чтение состояния и запись NVS.
- `/parameters`, `POST /echo`, `/private` — параметры, бинарное тело, Basic fixture.
- `/api/v2/info`, `/api/v2/summary`, `/api/v2/tasks` — совместимый CBOR v2 протокол
  общей демки; реальные runtime counters / stack high-water marks FreeRTOS.
- `/ws` — CBOR binary: summary 10 Гц и echo.request/echo.response; 4 клиента,
  сообщение до 4096 байт. `/api/v2/events` — диагностические SSE/JSON 1 Гц.
- `/api/v2/led`, `/api/v2/experiments` — защищённое управление LED CYW43439 и
  реальной задачей нагрузки π/CPU pulse (ядро 0 или 1, приоритет 1–2).
- `/api/v2/echo`, `/api/v2/stream`, `/api/v2/auth/status`, `/api/v2/auth/admin`
  — лаборатория HTTP и проверки 401/200/403.
- `/events` и диагностические SSE-маршруты — существующий общий SseTest.

Public Basic fixture: `demo` / `showcase-only`; это тестовые данные, не пароль Wi-Fi.
HTTP callbacks сериализованы. LittleFS читается непосредственно в блоки общей
очереди через `Response::sendOwnedSource`; буфера целого файла и ограничения 80 КиБ нет.
Два файловых контекста (NAWSERVER_DEMO_FILE_READERS) живут до завершения/отмены
передачи и освобождаются одним callback. Чтение файла и запись TCP выполняются
короткими шагами общего обработчика; следующий запрос и WS/SSE получают ход между ними.
Включены управляемые HTTP-буферы и keep-alive (два рабочих места, два idle-соединения).
Их отдельный лимит — 128 КиБ с ленивым выделением WS-арен; общий TX ограничен 32 КиБ.
Очередь: максимум 12 заданий / 32 КиБ payload, блок до `4 * TCP_MSS`, шаг до MSS,
до двух заданий соединения и один защищённый резервный слот. TCP допускает
частичную запись; это учитывает очередь. Текущие параметры и счётчики находятся
в `summary.transmission` и на общей INFO-странице рядом с CPU/RAM.
Лимит соединений — 10: четыре WS, до четырёх SSE и место для временных HTTP.

График и пул RAM показывают **FreeRTOS heap 192 КиБ**, а не весь физический SRAM.
Паспорт отдельно указывает SRAM 520 КиБ и отдельный newlib heap для C++/lwIP.
CPU0/CPU1 показываются отдельно на общей шкале и в истории задач. Общая загрузка
`cpu_busy_pct = (CPU0 + CPU1) / 2` нормирована на 100%; дополнительно API отдаёт
`cpu_sum_pct` на шкале 0–200%. Оба ядра действительно запущены.
RGB-лента, MCP, BLE runtime и пульт симулятора в этой демо не включены.

В SMP запись flash координирует постоянная RAM-задача `flash-guard` на CPU1
через официальный callback `get_flash_safety_helper`. Основные SDK/libc-задачи
закреплены за CPU0: newlib использует глобальный `_impure_ptr`, поэтому CPU1
не меняет этот указатель и выполняет только RAM-guard / чистую вычислительную
демо-задачу. Произвольный перенос SDK/libc-кода на CPU1 требует отдельной адаптации.

Короткие смешанные проверки выполняются на Pico 2 W, C3 и S3; длительный soak
отделён от этой проверки. Отдельный firmware verify/readback не выполняется.
[verification.md](verification.md) описывает исходную демо до общей очереди.

Зависимости сохраняют собственные лицензии: Pico SDK, FreeRTOS/его RP-порт,
CYW43 и LittleFS не перелицензируются лицензией NaWServer.

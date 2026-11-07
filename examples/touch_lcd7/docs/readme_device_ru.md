# Аппаратная конфигурация Touch LCD 7

Цель — Waveshare **ESP32-S3-Touch-LCD-7**, не 7B. Владелец указал Rev1.2.
На подключённом устройстве esptool определил S3 rev0.2, flash 16 МБ,
octal PSRAM 8 МБ. Идентификаторы/порт и полный backup остаются локально.

Аппаратные параметры сверены с [официальным ESP-IDF примером Waveshare](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7/tree/main/examples/ESP-IDF/08_lvgl_v8_demo)
и [описанием RGB LCD](https://docs.waveshare.com/docs/ESP32/ESP32-S3/ESP32-S3-Touch-LCD-7/Arduino/Arduino-RGB-LCD-Demo).
Реализация приложения самостоятельная; графика использует LVGL8.3.11,
vendor UI не подключён.
GT911 используется через драйвер Espressif `esp_lcd_touch_gt911` 1.1.3.

| Узел | Настройка |
|---|---|
| I²C | SDA8 / SCL9, 400 кГц |
| RGB | 800×480, RGB565, PCLK7 / DE5 / VSYNC3 / HSYNC46 |
| RGB D0…15 | 14,38,18,17,10,39,0,45,48,47,21,1,2,42,41,40 |
| Тайминги | PCLK12МГц; HPW/VPW4, HBP/HFP/VBP/VFP8; negative PCLK |
| CH422G | config0x24; output0x38; touch reset EXIO1, backlight EXIO2 |
| GT911 | vendor reset/address selection, GPIO4; драйвер polling |
| Буферы | два framebuffer в PSRAM, два SRAM bounce-буфера по16 строк, LVGL full_refresh |

Все операции LCD, touch и CH422G выполняются одним worker. HTTP только читает
копию состояния под mutex и кладёт команды в фиксированную очередь из8 элементов.
Одновременные обращения UI и HTTP не пишут в I²C или framebuffer напрямую.
Переполнение очереди возвращает503. Графика и LVGL pointer input также
обслуживаются этим worker; HTTP не вызывает LVGL.
Первый кадр LVGL рисуется в framebuffer1, пока RGB сканирует framebuffer0.
После submit нового кадра flush ждёт следующий on_frame_buf_complete, затем
следующий VSYNC, и лишь после этого сообщает LVGL flush_ready.
В bounce-режиме именно on_frame_buf_complete подтверждает смену bb_fb_index;
on_color_trans_done этого не гарантирует. Проверено по установленному SDK5.4.
Текущий IPv4 Wi-Fi отображается справа в верхней строке LCD; без активного
интерфейса — `IP: offline`. Чтение netif раз в секунду и изменение LVGL label
выполняются тем же worker, без отрисовки из Wi-Fi callback.
Таймаут500мс останавливает приложение через abort, а не разрешает опасное
повторное использование активного буфера. LVGL full_refresh рисует полный кадр
вне scanout; это не partial/direct-mode и не обещание минимального CPU.
Это тест экрана/касания, не готовый HMI движок.
PSRAM содержит framebuffer; XIP/cache настройки уменьшают конкуренцию с flash,
но устойчивость под нагрузкой требует отдельной аппаратной проверки.
Текущая ограниченная правка против редких артефактов и её проверки:
[RGB stability](readme_lcd_stability_ru.md). Визуальный дефект не считать
закрытым только по счётчикам кадров.

При ошибке инициализации worker останавливается, ready=false и команды503;
HTTP-диагностика остаётся доступной при наличии настроенной сети.
Нет автостирания NVS. CAN, RS485, SD, sensor AD и аккумулятор не управляются.

## Проверка 19.10.2026

Полные 16 МБ исходной flash считаны и сравнены с устройством командой esptool
verify-flash: digest matched. SHA-256, manifest и образ сохранены локально вне Git.

Первая offline-сборка: `pio run -e s3 -j 8` — SUCCESS, ESP-IDF5.4.0 / Espressif32 6.10.0,
Xtensa GCC14.2.0. Локальный `sdkconfig.h` подтверждает ESP32-S3,
flash16MB, octal PSRAM80МГц. Generic DevKitC-1 в PlatformIO задаёт SoC/toolchain;
его описание «N8 / no PSRAM» не является эффективной конфигурацией этого проекта.
Размер firmware.bin470976 байт; static RAM18896 байт; flash sections470576 байт.
SHA-256: `fa3e4cada37b66c5094acb85541e7daacc8e351e9f50e8c3811df58477d795eb`.
Сборка сделана без локальных Wi-Fi реквизитов: это offline-образ;
для доступа к веб-панели потребуется header и повторная сборка.
Node syntax check JavaScript страницы — PASS; длины bitmap-глифов проверены.

### Развёртывание по отдельному запросу владельца

S3 повторно идентифицирована перед записью; живая таблица разделов совпала
с таблицей из backup. `partitions.csv` приведён к исходным границам:
NVS0x9000/0x6000, phy_init0xf000/0x1000, factory0x10000/0x500000.
Таблица, bootloader и NVS не записывались и не стирались командой прошивки.
Локальный Wi-Fi header подготовлен для домашней сети, значения не выводятся
и не входят в Git. Backup сохранил прежний SHA-256 и остаётся ignored.

Сетевая сборка — SUCCESS; firmware.bin888096 байт,
static RAM37432 байт, flash sections887692 байт.
SHA-256: `bbfbc87df51131739e058a58b7319ba42d4ead9ff4004334f40a759af6bbf92c`.
Записан только app `[0x10000,0xe8d20)`; esptool подтвердил hash записанных данных.

- Boot — PASS: приложение touch_lcd7, PSRAM8МБ/80МГц, memory test OK.
- IP acquisition — PASS: домашняя сеть, адрес хранится только в ignored local/.
- LCD инициализация — PASS; владелец подтвердил видимое изображение на экране.
- GT911 — PASS: TouchPad_ID911, счётчик более20 физических касаний,
  реальные меняющиеся координаты, касания верхней полосы переключают pattern.
  Ошибок чтения touch за проверку0.
- Настоящий Edge — 12 PASS: загрузка UI, WS handshake/echo, пять режимов
  изображения с device readback, backlight off/on, отказы403/400,
  ready/PSRAM/touch_errors, отсутствие JavaScript ошибок.
- Дополнительный cross-site POST —403. HTTP200 и JSON source=device подтверждены.

Первый standalone APIRequestContext Playwright получил timeout при негативном
POST; устройство продолжало отвечать. Повтор через fetch самой страницы прошёл.
Отдельный HTTP cross-site запрос также прошёл. Этот короткий прогон не является
проверкой длительной нагрузки или всех сетевых сценариев.
Полный framebuffer screenshot устройства не снимался: сохранён screenshot
веб-панели, физическое изображение подтвердил владелец. Точность цвета каждого
режима и калибровка по углам отдельно не измерялись.
Логи, browser/touch отчёты, screenshot и deploy-state сохранены в ignored local/.

### LVGL и устранение мерцания

После первичной проверки владелец обнаружил сильное мерцание при движении пальца.
Первый renderer перерисовывал тот же единственный framebuffer, который читает
RGB scanout; первоначальный положительный статический показ не подтверждал
стабильность динамического изображения.

По запросу владельца прямое рисование заменено LVGL8.3.11, двумя framebuffer,
full_refresh и описанным выше ожиданием frame completion/VSYNC. Встроенные
шрифты Montserrat20/28 и LVGL pointer input заменили ручные bitmap-глифы.
I²C, touch polling, команды и HTTP/WS остались в отдельной S3 демке;
ядро NaWServer и C3 showcase ради этой правки не менялись.

Опора: [LVGL full-refresh double buffering](https://docs.lvgl.io/8.3/porting/display.html)
и [конкретный RGB backend ESP-IDF5.4](https://github.com/espressif/esp-idf/blob/v5.4/components/esp_lcd/rgb/esp_lcd_panel_rgb.c).

Сборка — SUCCESS: firmware1139904 байта, static RAM39016 байт,
flash sections1139504 байта. SHA-256:
`26448eac264f1f033968725bb96116adb5a1d1a4c89a0f6c31c56fad8e902c7b`.
Повторная идентификация S3 — PASS; запись только app `[0x10000,0x1264c0)`,
esptool hash verification — PASS. Boot/IP/GT911 — PASS.

Владелец после прошивки явно подтвердил: «Да, изображение стабильное» при
движении пальца. После обновления зарегистрированы7 физических касаний,
touch_errors0 и141 завершённое переключение кадров уже в первом status-снимке.
Последующий30-секундный прогон:78 HTTP-снимков,1156 frame events и1156 VSYNC,
без регресса uptime/счётчиков, перезапуска или ошибок touch; min наблюдаемой
свободной внутренней RAM171683 байта. Это короткая проверка, не длительный soak.
Edge regression —12 PASS, включая все цвета/сетку, подсветку, WS echo,
валидацию команд и отсутствие JS ошибок. JSON подтверждает renderer=lvgl-8.3.11,
framebuffers=2 и живые счётчики. Отчёты: ignored local/lvgl-*.json/log.
Исходный полный backup сохранён и исключён из Git.

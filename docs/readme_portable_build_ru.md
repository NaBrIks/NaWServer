# Переносимая сборка и синхронизация

Дата: 2026-11-03. Общие исходники NaWServer выбираются из одного явного реестра
`cmake/NaWServerSources.cmake`. Реестр группирует HTTP, wire codecs, TCP runtime,
WS, SSE и admission. Наличие файла в каталоге само по себе не включает его в
прошивку. ESP-IDF component, host tests и RP используют этот реестр.

## Обычный CMake

Для Windows/Linux можно подключить корень через `add_subdirectory` и связать
приложение с `NaWServer::NaWServer`. При самостоятельной конфигурации доступны
`NAWSERVER_WEBSOCKET`, `NAWSERVER_SSE`, `NAWSERVER_TCP_MANAGED`, `NAWSERVER_AUTH`, `NAWSERVER_CLIENT_QUOTAS` и выбор
`NAWSERVER_PLATFORM`. Автоматически определяется только host Windows/Linux.

Для нескольких явно составленных целей можно подключить
`cmake/NaWServer.cmake` и вызвать:

```cmake
nawserver_add_library(device_server PLATFORM pico)
target_link_libraries(application PRIVATE device_server)
```

Функция принимает `PLATFORM windows|linux|pico`, флаги `NO_WEBSOCKET`, `NO_SSE`
и `MANAGED`, `NO_AUTH`, `NO_CLIENT_QUOTAS`. Без MANAGED исключается управляемый connection manager; policy и аутентификация независимы.
Выбранные compile definitions распространяются на потребителей цели, поскольку
платформенный выбор синхронизации влияет на размещение внутренних членов классов.

Pico SDK и FreeRTOS инициализирует приложение до подключения цели. Оно предоставляет
`FreeRTOSConfig.h`, `lwipopts.h`, Wi-Fi, credentials, монтирование FS, Flash-разметку,
аллокаторы и политику размещения задач. Библиотека их не подменяет.

## Нижний уровень

| Часть | Реализация |
| --- | --- |
| Windows/Linux transport и worker | Существующие отдельные платформенные файлы |
| ESP-IDF transport и worker | Платформенные IDF-адаптеры sockets/worker; native HTTPD удалён |
| Standalone lwIP sockets | `backends/lwip_tcp.h`, private `platform/lwip_tcp.cpp` |
| RP2350 FreeRTOS worker | Private `platform/pico_freertos_worker.cpp` |
| Mutex | SDK-free `detail/synchronization.h`, выбранная private реализация |

`NAWSERVER_LWIP_SOCKET_COUNT` задаёт проверяемый диапазон native descriptors;
по умолчанию используется `MEMP_NUM_NETCONN`. BSD alias macros должны быть
выключены (`LWIP_COMPAT_SOCKETS=0`), `timeval` и errno согласованы с libc.
Для проверенного Pico SDK профиль явно переопределяет `LWIP_PROVIDE_ERRNO`,
иначе lwIP и прикладной C++ могут наблюдать разные errno.

RP-worker использует upstream stack depths в элементах стека. ESP-IDF принимает
байты. Для роли Transmission предусмотрено 1024 элемента на RP и 4096 байт IDF.
Текущая RP-политика оставляет server/SDK/libc задачи на CPU0; это не разрешение
переносить произвольную libc-зависимую работу на CPU1.

## Mutex и GNU C++ runtime

Общий код использует внутренние `detail::Mutex`, `LockGuard`, `UniqueLock`.
В штатном host/IDF варианте Mutex соответствует `std::mutex`. При
`NAWSERVER_FREERTOS_MUTEX=1` используется статически размещённый FreeRTOS mutex,
без динамического выделения semaphore. Конструктор создаёт его до первого
использования, поэтому нет гонки ленивой инициализации.

Публичное внутреннее объявление не включает FreeRTOS headers: оно содержит
непрозрачную память, по умолчанию 128 байт, с выравниванием max_align_t.
Private реализация проверяет sizeof/alignof реального `StaticSemaphore_t` и
необходимые `configSUPPORT_STATIC_ALLOCATION`/`configUSE_MUTEXES`.
`NAWSERVER_MUTEX_STORAGE_BYTES` можно изменить только согласованно для всей
сборки. Эти блокировки предназначены для задач, не для ISR.

У поставляемого bare Arm GCC 14.2.1 есть отдельное ограничение: без `__GTHREADS`
libstdc++ выбирает single-threaded счётчики `shared_ptr`. Поэтому устранение
использования std::mutex не позволяет просто убрать всю интеграцию gthread.
В `detail/libstdcxx_freertos/bits/gthr.h` сохранён явно выбранный мост для
проверенного toolchain. Он включает threaded/atomic модель счётчиков и узкий
набор GNU mutex hooks. Установленные файлы компилятора не изменяются.

Это не полноценный порт std::thread, condition_variable, call_once и всех
операций с таймаутами. Неподдержанные hooks намеренно не имеют реализации.
Полное удаление моста требует thread-aware toolchain либо изменения владения
объектами, включая shared_ptr. Независимо остаётся необходимой корректная
интеграция newlib/libc с RTOS; Mutex её автоматически не обеспечивает.

## Проверенные сборки

- Root standalone Linux library, Zig 0.14.1/Clang 19.1.7: полный набор WS/SSE/
  admission собран в `local/portable-standalone`.
- Root standalone Linux без WS/SSE/admission: собран в
  `local/portable-standalone-minimal`.
- RP2350 ARM/Pico SDK 2.3.1/FreeRTOS 11.3.3, RP2350_ARM_NTZ community port:
  собран через общую цель. Перенесённые lwIP/worker/Mutex также отдельно
  прошли компиляцию. Точный размер итогового образа зависит от параллельных
  изменений ресурсов и сервера, поэтому фиксируется в отчёте приёмки.

Аппаратные проверки и другие SDK/ports учитываются отдельно. Эти строки не
являются заявлением о поддержке всех версий FreeRTOS или всех lwIP SDK.

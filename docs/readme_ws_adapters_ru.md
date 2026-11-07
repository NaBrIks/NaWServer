# Общий WebSocket и адаптеры — 24 октября 2026

> Исторический отчёт о версии 24.10.2026. С 03.11.2026 нативный ESP-IDF HTTPD
> удалён. Его типы, lifecycle и приведённые ниже проверки относятся только к
> указанному старому checkpoint; это не инструкции для текущей сборки.
>
> Актуально: ESP-IDF, Windows, Linux и RP используют `TcpHttpBackend` с адаптером
> sockets своей платформы. Маршруты объявляются через `routes()`, ранние
> auth/quota middleware выполняются до тела и upgrade. `Drop` передаёт отмену
> владельцу I/O; WS/SSE сохраняют lease ресурса до закрытия соединения.
> SSE подключается как `sse::TcpAdapter(http_backend)`; native HTTPD async API нет.
> См. [переносимую сборку](readme_portable_build_ru.md),
> [текущий SSE API](readme_sse_ru.md) и
> [общую очередь SSE](readme_sse_shared_transmission_ru.md).

HTTP checkpoint: `5a09364`, ветка `codex/http-backend-adapters`.
WebSocketEndpoint/WebSocketClient теперь собираются без ESP-IDF. Приложение
сохраняет onConnect/onMessage, send/send_binary, broadcast и таблицу handlers.
Новый backend не требует замены прикладных обработчиков или HTTP pipeline.

## Граница

| Общая часть | Адаптер |
|---|---|
| WebSocketEndpoint: допуск, callbacks, список клиентов и broadcast | HTTP upgrade и регистрация native routes |
| WebSocketLimits: размер сообщения, pending, число клиентов | Разбор/сборка wire frames и control frames |
| WebSocketClient: проверка аргументов и результат отправки | Копирование payload, очередь, сетевой ввод-вывод |
| WebSocketSession: owned token, sendCopy/invalidate | Привязка токена к конкретному соединению |
| WebSocketExchange: opening/client/attach/readMessage/close | Память входящего сообщения на время callback |

HttpRequestHandler::transportRoutes возвращает заимствованную неизменяемую таблицу.
IDF регистрирует точные native WS URI перед wildcard HTTP; Windows находит endpoint
при upgrade. Оба вызывают тот же HandlerBase::handleRequest. Это сохраняет и
прикладные подклассы WebSocketEndpoint. Request передаёт WebSocketExchange через
transportExtension; SDK pointer общему WS не нужен. SSE остаётся на своём IDF runtime.

На ESP32 используются HTTPD handshake, frame API, socket sessions/free_ctx и
httpd_queue_work. Собственных SHA-1, HTTP parser или RTOS-обёртки для ESP32 нет.
Нативная реализация находится в src/backends/esp_idf_websocket.cpp.

Windows использует Winsock, системные SHA-1/Base64 и один select loop с
последовательными callbacks. Открытый WS не занимает HTTP listener. Очередь и
входные сообщения ограничены; частичная отправка хранит offset и имеет deadline
одну секунду. HTTP остаётся loopback-only, Connection: close. Это диагностический
backend для браузера, не новая production HTTP/TLS-библиотека.

MemoryHttpBackend работает с готовыми сообщениями, без handshake/wire parser:
connectWebSocket(path), receive(handle, bytes, binary), takeSent(handle), close(handle).
Handle содержит generation, поэтому старое соединение не становится новым после
end/begin. Callback, endpoint и результаты отправки проходят общий код.
Этот backend синхронный, однопоточный; он не эмулирует планировщик или TCP.

## Владение и остановка

Конструкторы связывают зависимости; begin запускает runtime после SDK/network init.
Handlers и endpoints должны жить до успешного end. Client pointer и принятые bytes
заимствованы только на время callback. Producer использует endpoint broadcast;
указатель WebSocketClient за пределами callback сохранять нельзя.

sendCopy копирует данные до возврата. Queued означает приём адаптером, не доставку.
По умолчанию WebSocketLimits задаёт 4096 bytes, четыре pending messages и четыре
клиента на endpoint. Нулевые лимиты отвергаются. QueueFull возвращает отказ;
автоматическое закрытие по переполнению этим этапом не добавлено.

Перед end приложение останавливает producers. IDF закрывает приём новых заданий,
ждёт ранее принятые до пяти секунд, затем вызывает httpd_stop. При отказе stop
сохраняет handle и контексты для повторного end. Windows join завершает callbacks
и освобождает клиентов; memory закрывает их синхронно. Из callback end не вызывают.

Аутентифицированный WS и TLS/WSS не реализованы: HTTP Route с auth не защищает
native WS upgrade. SDK сохраняет свои ограничения wire protocol; одинаковая
модель сообщений не означает одинаковое поведение каждого некорректного wire frame.

## Проверки этой реализации

- Memory WS: 41 checks — text/binary/NUL/empty, copied send, queue-full,
  broadcast/admission, invalid input, stale handles, HTTP рядом с WS, restart.
- Native transport harness: 61 checks — static MIME и прежние send errors,
  pending/drain, fragmented receive с interleaved ping, предел сообщения,
  invalid continuation, disconnect до выполнения отправки.
- IDF lifecycle model: WS=0 —207 checks; WS=1 —234 checks. Модель SDK отдельно
  от native сборки/железа; значения HTTP methods соответствуют pinned SDK.
- Windows: handshake по RFC example, HTTP при открытом WS, producer copied sends,
  binary/NUL, fragmented/coalesced frames с ping, close, invalid frames,
  reconnect/end/begin. Лимит8192: echo5000/8192 и отказ объявленной длины8193;
  прежний default4096 сохранён. Финальный тест1048 checks,10/10 повторов PASS.
  Частичные socket writes поддержаны, но принудительно
  короткая запись в тесте не моделировалась.
- Полный host CTest:16/16 PASS (Zig0.14.1, C++17, warnings-as-errors для backend/core).
  Windows Edge mini UI14/14 и повтор PASS; CSS отдельным ресурсом,
  light/dark и430/1180px, JavaScript errors отсутствуют.
- Реальная C3: pinned SDK build PASS; записан только app0 (925936 bytes).
  Startup 89 response/basic, loopback63/63, lifecycle27/27; LAN smoke с WS
  reconnect PASS и198 HTTP checks PASS. Edge mini UI14/14 и повтор PASS;
  light/dark,430/1180px. Отдельный CSS, общие frontend assets.

Raw evidence, firmware, device identity и адрес остаются в ignored
local/idf-c3-hardware/*-ws*; browser screenshots — browser-ws/.
Ранний Windows limit-test один раз оборвался на receive. При отправке всего
отвергнутого payload закрытие с непрочитанными данными может закончиться TCP reset;
новый тест проверяет ранний отказ по объявленной длине без отправки payload.
Это возможное объяснение, не доказанная трассировкой причина разового отказа.
Надёжная доставка close-кода при произвольном некорректном потоке не заявляется.
Это короткая acceptance, без soak, continuous-producer shutdown или других SoC.
Парсеры URL/form, CBOR, утилиты и продуктовая логика этим этапом не изменены.

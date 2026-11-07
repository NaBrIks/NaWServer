# SSE в библиотеке: проверка 19 октября 2026

Интеграция согласована с основным HTTP-чатом. Публичный `src/sse.h` предоставляет
`NaWServer::sse::EventSource`; единственный runtime находится в `src/sse` и явно
включён в root CMake компонента. `tools/sse` компилирует/линкует этот runtime для
тестов, собственной копии нет. Пример приложения — `examples/sse`.
Существующие Request/Response и hooks не менялись для интеграции.

## Сборки и host

ESP32-C3, ESP-IDF 5.4.0, RISC-V GCC 14.2, C++17. Успешно собраны:
`tools/sse/integration_device`, `examples/sse`, `examples/compile_check` (c3),
`examples/minimal` (c3). Другие SoC не проверялись.

Общий CMake/CTest: **8/8 PASS**, включая canonical SSE sources. Encoder/session:
**20 269** проверок; bridge/headers/pipeline: **712**. Проверены закрытое владение
до activate, rollback, auth/policy, duplicate send, post-next error, sticky External,
ограничения/инъекции/дубликаты headers, exact 768 bytes и overflow. Header encoder
проверяет длины до записи, не размещает второй Frame на стеке; alias входа с
выходом отклоняется без изменения результата.

## Физический C3, LAN, браузер, stop

Чип/порт/MAC повторно проверены непосредственно перед каждой записью. Запись только
app0; data partitions с 0x290000 не менялись, backups и erase-all не выполнялись.

- Integration binary: **818 976 bytes**, SHA-256
  `efc6347bb0af459b2228d7a476a6050b2fb59e24a31700c5af1dc5c9496132c2`.
  Записан диапазон `[0x10000, 0xd7f20)`.
- На C3 выполнены те же **20 269** encoder/session checks.
- LAN: **2 579 protocol checks PASS**; console 2 581 дополнительно считает две
  проверки сохранения JSON. Число polling checks зависит от времени выполнения.
  41 async begin / 41 complete, rollback 1.
- Реальный браузерный EventSource: **PASS 3** — UTF-8/multiline, empty data с id
  reset, встроенный NUL. Скриншот сохранён в ignored `local/`.
- Stop с четырьмя активными подписками и pending frames: **74 checks PASS**.
  Итог: **46 begin / 46 complete**, rollback 1, active 0, pending bytes 0,
  close errors 0; free heap после stop 160 552 bytes.

LAN также проверяет 401/403 без async begin; Last-Event-ID absent/empty/limit;
GET с body; настоящий rollback с последующим обычным 409; четыре подписчика и
отказ пятому; UTF-8/empty/NUL для всех; GlobalFull; 30 reconnect; остановившего
чтение клиента рядом с двумя здоровыми; static HTTP/WS во время SSE; individual
overflow и отсутствие второго HTTP ответа после External.

Числа финального LAN: heap после трёх серий churn **147 480 / 146 844 / 146 844**;
mixed HTTP/WS **107 ms**; максимальный измеренный publish **182 us**; slow reader
получил 101 Queued перед stall; overflow 1, stall 1. Это конкретный короткий прогон,
не WCET, latency SLA или доказательство длительного отсутствия утечек. Для raw TCP
reader запросили SO_RCVBUF=1024; фактическое окно ОС не сертифицировано.

HTTPD minimum free stack: **900 bytes** при stack 6144; первый библиотечный прогон
до удаления временного header Frame показывал 596 bytes. Это minimum за весь
fixture, включая низкоуровневый rollback route; запас для других application
callbacks следует измерять отдельно. Стек HTTPD и HTTP API при исправлении не меняли.

`sizeof(EventSource)` на C3 **12 944 bytes**, плюс один worker со stack 4096 и SDK
allocations. Integration fixture имеет дополнительный test-only IdfAdapter для
реального scoped rollback: ещё один такой объект/worker. Обычному EventSource
второй worker не требуется; probe не публикует и не обслуживает подписки.

## Возвращённая прошивка и HTTP/WS регрессия

После SSE stop пересобран и восстановлен **minimal**, SHA-256
`57f0ec531b880f91d19ff99825d10becd3ae6f4af874ab926a7c1c1b54ad817a`,
903 072 bytes, `[0x10000, 0xec7a0)`. Хеш совпал с ранее проверенным minimal:
неиспользуемый SSE runtime не запускается и удаляется linker GC.

On-device: **85 response/Basic**, **63 loopback**, failed 0, HTTP server ready.
Затем успешно выполнены:

```powershell
go -C tools/smoke run -buildvcs=false . 192.0.2.6
go -C tools/http_api_smoke run -buildvcs=false . http://192.0.2.6
```

Первый прогон проверил HTTP/forms/Basic/DELETE, partial/body limit/health,
raw/copied/NUL/binary 0..4096 и WS text/binary/reconnect. Второй — **198 HTTP
response checks**, включая 2/4 concurrent clients. Его snapshot: free 198 772,
minimum heap 169 560, largest block 114 688, HTTP-stack-min-free 2092 bytes.

**Финальное состояние:** плата работает на minimal по `http://192.0.2.6`,
COM10 освобождён; SSE service сейчас не запущен. Для SSE нужно приложение с явным
start и route, например examples/sse. Устройство/репозиторий обновлены в локальный монитор стенда.

## Ограничения

Проверен plain native HTTPD 5.4; TLS, send overrides, внешнее forced close активных
async requests, SDK OOM fault injection и длительный soak не проверены. Правильный
порядок: остановить producers/admission → успешно EventSource.stop → server.end.
В API не сохраняются Request/Response/Next/application callback.


Машиночитаемые результаты: `tools/sse/results/library-c3-summary.json` и
`tools/sse/results/library-source-sha256.json`. Raw serial, адреса оборудования,
Wi-Fi secrets и binaries остаются ignored в local/ и build folders. Исторические
reports/hashes первого tools-only и bridge этапов не перезаписывались.

После очистки SSE-документации 19 октября 2026 аппаратный прогон не повторялся;
измерения выше относятся к указанному firmware hash. Текущие результаты проверки
документации и source fingerprints записаны отдельно в
`tools/sse/results/cleanup-verification.json`.

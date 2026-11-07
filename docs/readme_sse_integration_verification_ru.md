# SSE bridge: C3, LAN и браузер, 19 октября 2026

Это историческая проверка перед включением в компонент. Текущий runtime:
`src/sse`, публичный `sse.h` / EventSource; [библиотечный отчёт](readme_sse_library_verification_ru.md).
Приведённые ниже пути и hashes сохранены для воспроизводимости прежнего прогона.

Реализованы reserve/claimExternal/activate/rollback и bounded owned headers в
`tools/sse`. Пример `tools/sse/integration_device` линкует **настоящие** Request,
Response, Route, AuthMiddleware/FixedBasicAuthenticator и HTTPD компонента.
Core и hooks реализовал основной HTTP-чат; SSE-чат core не менял.
[Контракт интеграции](readme_sse_integration_ru.md).

## Результаты

- Host: **20 269 CHECK** encoder/Sessions, **708 CHECK** bridge, 0 ошибок.
  Bridge проверяется с настоящими portable Response/pipeline/AuthMiddleware;
  подменён только native backend для управляемых отказов и наблюдения порядка.
- Тот же encoder/Sessions на физической C3: **20 269 CHECK**, 0 ошибок.
- LAN: **2551 CHECK** в числовом JSON, 0 ошибок. Консольный итог 2553 включает
  ещё две проверки сериализации/сохранения JSON после фиксации его счётчика.
- Браузерный EventSource: **SSE PASS 3** — UTF-8/multiline, empty/id reset, NUL.
  Результат прочитан через UI, screenshot: ignored `local/sse-browser-pass.png`.
- Stop с четырьмя active streams и pending event: **74 host CHECK**, 0 ошибок.
  Итог устройства: **86 begin / 86 complete**, rollback=2, active=0, pending=0.
  Firmware проверила close_errors=0 и остановила HTTPD перед DONE.

## Сценарии LAN

1. Настоящие auth/policy дают 401/403 до SSE bytes, native begin не растёт.
   Last-Event-ID непустой/пустой/отсутствующий различаются, 129 bytes дают 400.
   GET с body отклоняется без handoff.
2. DefaultHeaders, NoStore/NoSniff и локальная строка override доходят до wire
   после возврата callback, MIME остаётся SSE.
3. Scoped reservation без claim откатывается: исходный callback выдаёт обычный
   **409 reservation rolled back**, native copy завершена.
4. Error после next и duplicate send после handoff закрывают поток, не выдают
   fallback 500/обычное тело; owned request завершается ровно один раз.
5. 2/4 подписки, отказ пятой с 503; четыре получателя получают UTF-8, multiline,
   trailing LF, event/id/retry, empty/id reset и NUL. Реальные Route используют
   Request/Response и Context формы endpoint.
6. Burst=3 четырём сессиям даёт GlobalFull без IndividualOverflow/закрытия клиентов.
7. Три серии по десять reconnect/close: heap 164608, 164396, 164396 bytes.
   Дальнейшего накопления после прогрева на этой короткой серии нет; это не soak.
8. Остановившийся читатель плюс две нормальные SSE. Следующий большой Frame
   публикуется только после ухода предыдущего из очереди. Зарегистрирован
   **Stall**, overflow-счётчик не изменился; две другие сессии остались открыты.
   Во время pending передачи проверены static binary 4096 bytes, HTTP health,
   core WS handshake/masked echo и правильные события двум нормальным SSE.
9. Отдельный burst=4 одному клиенту даёт IndividualOverflow и close этой сессии.
10. Application owner task выполнил SSE drain до server.end с четырьмя active
    SSE и pending event. Все клиентские потоки получили EOF/close в timeout.

Нормальный static+WS запрос суммарно **116 ms**; максимальный измеренный publish
**145 μs**, включая возможное планирование. Это наблюдения, не WCET. Test limits:
heartbeat=500 ms, stall=1000 ms, event deadline=3000 ms; 256 bytes/step, одна task.

## Первый неуспешный опыт и счётчики

Первый LAN-запуск не достиг stall за 20 s после остановки чтения приложением.
Он **не засчитан как PASS**; после выхода клиента requests завершились: 40/40.
Лог сохранён отдельно: ignored `local/sse-integration-lan-first-failure.log`.
В test client добавлен SetReadBuffer(1024). Эффективный TCP window/OS buffer не
измерялся; 1024 — запрошенное значение. С тем же firmware полный повтор прошёл:
101 Frame принят в очередь до Stall. Это не гарантированная ёмкость SSE очереди.

Успешный повтор добавил **41 begin / 41 complete** к прежним 40: итого 81/81,
rollback=2, stall=1, overflow=1. Browser добавил одну подписку, stop — ещё четыре:
**86/86**. JSON с 81/81 — снимок до browser/stop; serial DONE с 86/86 — конец того
же boot. Два rollback относятся к двум попыткам smoke, а не к одному повтору.

## Сборка, flash и освобождение платы

Основной HTTP-чат явно передал C3 после своих проверок. Перед записью заново
определены USB device/chip/MAC: C3 rev0.4 и ожидаемая identity подтверждены.
MAC/credentials не включены в артефакты; использован existing ignored Wi-Fi header.

espressif32@6.10.0, IDF 5.4.0, GCC RISC-V 14.2.0+20241119, C++17, -Os,
160 MHz, tick 1 ms. Только C3. Firmware.bin **818496 bytes**, SHA-256:
`8e33328889f18fb6c4f5793fb666480b1de3eaf7694a533cc9715653631d795e`.
Запись `[0x10000,0xd7d40)`, erase sectors `[0x10000,0xd8000)`, внутри app0
`[0x10000,0x290000)`. Esptool подтвердил hash. Bootloader/partition table/данные
не прошивались, backup не создавался. NVS не стиралась; Wi-Fi storage RAM.

Вся test firmware: static RAM **43240**, flash по линкеру **818096 bytes**;
это включает core/Wi-Fi/static/browser fixtures, не прирост одного SSE.
sizeof(IdfAdapter)=**12944 bytes**, task stack=4096 bytes плюс SDK allocations.
Собственные frames: worker 688, bridge 1136, header encoder 928 bytes;
это не полные call-chain peaks. Heap после stop **171772 bytes**; работающий
сервер и остановленный сервер — разные baseline.

Capture закрыт, доступность COM10 проверена коротким open/close без RTS/DTR.
**COM10 освобождён**, SSE HTTPD остановлен. Основной чат восстанавливает minimal
сам; SSE-чат minimal не прошивал и после освобождения плату не использовал.
Здесь не утверждается текущая прошивка после восстановления основным чатом.

## Ограничения

Автоматический browser reconnect после сетевого разрыва не инжектировался:
browser проверил реальную семантику событий/id, LAN client — explicit reconnect
и Last-Event-ID. TLS, external forced close active async request, SDK heap OOM
fault injection, длительный soak и другие SoC не проверены. Publish Queued и
socket acceptance не подтверждают обработку события приложением браузера.

Numeric summary и source fingerprints: `tools/sse/results/integration-*`.
Raw serial/flash/LAN logs, binaries и screenshot — ignored local/ и .pio/.
LICENSE, source list core и чужие dirty
изменения сохранены; staging/commit/push не выполнялись. Старый loopback отчёт
остаётся историческим: его source hashes не подменены новыми.

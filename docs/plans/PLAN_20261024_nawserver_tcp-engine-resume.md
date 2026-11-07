# Точка продолжения на 25 октября: TCP HTTP/WS

Обновление25.10: работа возобновлена, P5 завершён (Windows44/44, Linux41/41,
browser18/18, sanitizer/runtime и два hour fuzz jobs). Текущий этап — P6.
См. [отчёт](../readme_tcp_engine_verification_ru.md) и checkpoint
ignored `local/tcp-engine-p5/`. Ниже сохранена исходная ночная точка без
переноса её исторических результатов в свежую матрицу.

Сохранено24.10.2026 по прямой просьбе владельца закончить на сегодня.
**P0–P2 завершены; P3 не начат.** Ночью работа не продолжается. Агенты завершены,
собственное TCP demo завершилось, автоматизаций и ночных тестов нет.

## Checkout и сохранность

- `<repo>`, HEAD `63ee25c55a332ce8078df625da194c9f48cebaa1`.
- Commit/push не выполнялись. Дерево намеренно dirty, новые sources ещё untracked.
- До работы уже были owner changes в AGENTS, architecture EN/RU, code_style EN/RU,
  docs/README, uncrustify.cfg и четыре новых TCP ADR/research/plan документа.
  Не сбрасывать их вместе с реализацией.
- Исходный diff/fingerprints: ignored `local/tcp-engine-p0/`, tracked diff SHA256
  `135B7E14738FD0611983B93E7EF3DCC4F2CA33FBEBF8D30EC9062A953E8CFFEC`.
- Полные checkpoints с новыми файлами: `local/tcp-engine-p1/`, `local/tcp-engine-p2/`.
- Хэши исходных owner code_style EN/RU и uncrustify.cfg при завершении совпали.
  Architecture EN/RU обновлены синхронно; завтра вновь проверить пары/owner edits.

## Завершённое сегодня

Подробности: [поэтапный отчёт](../readme_tcp_engine_verification_ru.md),
[TCP HTTP guide](../readme_tcp_http_ru.md),
[основной план](PLAN_20261024_nawserver_tcp-engine-implementation.md).

P0: исправлена test-only SSE EOF/completion race без произвольного sleep;
100/100 SSE repeats и18/18 host tests PASS. Фоновый HTTPD/C3 baseline без больших
unit fixtures: IDF5.4.0/GCC14.2 build PASS; Flash888784 B/static RAM42544 B.
Это сборка, **не hardware**. C3 сегодня не прошивалась, COM не использовался.

P1: узкий tcp::Transport, Winsock, generation/ownership slots и scripted transport;
21/21 host tests PASS,100 restarts, бинарные/half-close/reset/stale-token cases.

P2: общий parser/writer, SessionIo, TcpHttpBackend/HttpExchange, private Windows
worker; тот же dispatcher/routes/middleware. Root frontend и HTTP demo composition
общие через `host/http_demo.h`. Native IDF HTTPD и прежний Windows WS/SSE сохранены.
Owned RequestBody fallback пока остаётся; prepared-body optimization — P4.

Финальные проверки:

- Полная host build PASS; **CTest28/28 PASS,9.50s**.
- Uncrustify32 затронутых C/C++ files PASS; git diff --check PASS.
- Parser29550 checks/5826 splits, writer63623, SessionIo344,
  scripted actual HTTP1107/79 exchanges, TCP HTTP socket corpus120,
  lifecycle374/10 restarts, worker440/100 cycles PASS.
- Реальный Edge:12/12 fetch и повтор PASS, desktop/mobile/dark проверены,
  browser exit0/demo exit0. Собственный timed demo штатно завершился.
- Реальный C3 GCC:5/5 common objects +sizeof probe PASS, GNU++17/-Og,
  noexceptions/nortti, **без SDK headers/link/runtime**.
- C3 start storage subtotal9032 B (8.820KiB) до worker/allocator/body/maps;
  executeHttpConnection compiler frame1200→208 B после extraction rejection.
  Это compiler frame, не аппаратный stack high-water.

Точные evidence:

- `local/tcp-p2-final-build.log`, `local/tcp-p2-final-style.log`,
  `local/tcp-p2-final-suite.log`, `local/tcp-p2-browser-final/summary.json`.
- **Авторитетный профиль compiler frames:** `local/tcp-p2-c3/og-common/summary.json`.
  Соседние вспомогательные profiles с ним не смешивать.
- Baseline SDK: `local/tcp-engine-p0-c3/README.md`, summary.json и actual SCons flags.
- Checkpoint P2 содержит source snapshots/fingerprints и копию final LastTest.log.

Host: Zig0.14.1/Clang19.1.7, CMake4.0.2, Ninja, C++17. Актуальный build directory
сохранил имя P0:

```powershell
Set-Location (git rev-parse --show-toplevel)
cmake --build local/tcp-p0-host -j 4
Push-Location local/tcp-p0-host
try {
	ctest --output-on-failure
}
finally {
	Pop-Location
}
```

Browser: запускать **собственное** `nawserver_tcp_demo 0 15000`, читать его
ephemeral loopback address и передавать в `tests/browser_http_check.cjs`.
Чужое demo PID7976 при завершении оставалось живым; завтра заново идентифицировать
процесс, не завершать его по историческому PID и не перезаписывать старый executable.

## С чего продолжить: P3

1. Сверить HEAD/status, owner edits и пары архитектуры/стиля. Прочитать AGENTS и
   четыре TCP документа; продолжать существующий checkout, сохраняя dirty work.
2. Общий WS handshake/frame codec: portable SHA-1/Base64 с Go oracle,
   shared UTF-8 helper без копии SSE runtime, fragmentation/control/close corpus.
3. Prepare/commit/rollback admission **до101**, onConnect только после полного
   handshake. Сначала изучить нынешний WebSocketEndpoint::handleRequest: сейчас
   session/client/container создаются в opening callback после upgrade.
4. TCP WS sessions и SSE Stream/worker: один I/O owner, tokens/generations,
   reserve/activate/rollback/finish, сохранение prefetched bytes и owned metadata.
5. HTTP+WS+SSE, partial writes, stop/drain/retry и browser WS/EventSource;
   затем отдельный P3 checkpoint. Authenticated WS пока unsupported.

Не перескакивать в P4 pools/body optimization, P5 Linux, P6 SDK или P7 load/soak.
Независимые части распределять между агентами; source **и evidence directories**
назначать непересекающимися, затем выполнять взаимные reviews.

## Непроверенное и ограничения

- Linux adapter/runtime ещё нет. WSL показал только stopped docker-desktop;
  ordinary isolated Linux distro не подтверждена.
- ESP-IDF TCP adapter/worker, Kconfig/backend choice и TCP-only source/dependency
  lists ещё нет: корневой IDF CMake пока требует HTTPD. Это P6.
- Матрица: C3/IDF5.4.0 и6.0.3, оба backend. Установленные5.4.4/6.1.0 её не заменяют;
  IDF6.0.3 не найден локально. SDK-сборки запускать в фоне.
- Новых firmware runtime, hardware, load/fuzz/sanitizer/soak результатов нет.
  Перед flash вновь идентифицировать разрешённую C3 и intended ranges;
  без backup и дополнительных post-flash readback/hash passes.
- Linux рядом с ioBroker только после isolated gate и runbook с реальными quotas/
  health; предельные stress/fuzz/OOM туда не переносить.
- Stop timeout сохраняет зависимости. Не уничтожать owners до успешного end.
  Ambiguous retained Fatal close удерживает cleanup без слепого retry;
  known WouldBlock и отсутствие предыдущей close attempt различаются.
- P3 обязан сохранить single I/O owner и добавить короткую синхронизацию table
  без network I/O под lock. Таблица сама mutex не создаёт.
- Не заявлять hardware support, zero-heap, MCU stack headroom или performance.

Возобновлять работу после нового поручения владельца.

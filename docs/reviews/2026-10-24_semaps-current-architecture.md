# SeMaps: сверка шести карт с текущим кодом

24.10.2026, рабочее дерево `<repo>`.
Поручение владельца: обновить все шесть карт через MCP и синхронизировать
документацию архитектуры. Исходники firmware/frontend не изменялись этой задачей.
Предшествующие изменения рабочего дерева сохранены.

## Содержание ревизии

Существующие project/view/entity IDs сохранены. Проект MCP по-прежнему
`nawserver`; названия границы и слоя теперь NaWServer. Модель авторская:
`graph_status` возвращает `extractors=[]`, C++ extractor не настроен.
Факты сверены чтением текущих `include/NaWServer`, `src/http`, `src/auth`,
`src/routing`, `src/websocket`, `src/sse`, CMake и профильных руководств/отчётов.
Записи registry/view напрямую не редактировались.

Добавлены 15 сущностей и 18 связей. Live registry содержит 94 авторские
сущности и 52 связи. Исправлены устаревшие описания `void send`, IDFWebServer,
прежние пути исходников, «SSE исключён» и «следующий HTTP шаг — плата».

| View | Изменение | Placements | Видимые связи | Скрытые связи |
|---|---|---:|---:|---:|
| `v_layers` | Request/Response раздельно, EventSource в библиотеке, SSE→HTTPD |19|7|3|
| `v_context` | HTTP/WS/SSE снаружи; application ownership и новая граница |15|5|4|
| `v_tasks` | SSE admission на HTTPD, отдельный worker для send/complete |18|6|1|
| `v_flow` | WS results/cleanup и отдельный SSE handoff/Frame/worker flow |18|12|0|
| `v_app` | Явный опциональный SSE start и обязательный успешный drain до end |16|8|2|
| `v_access` | Авторизованный SSE Route, актуальные body/auth границы |17|8|1|

Placements включают контейнеры. Связи отфильтрованы по вопросу каждой карты,
остаются в registry и доступны через get_relations/get_view(hidden=true).
В слоях скрыты дополнительные calls handlers→Response/SSE и producer→SSE;
в границе — внутренние handler calls/dependency и дополнительный producer→SSE.
Поток копирования SSE раскрыт на v_flow, доступ — на v_access. На v_tasks
producer→SSE линия скрыта, чтобы не смешивать рисунок execution с полным data-flow.
На v_app прямые register→begin и producers-stop→end скрыты в пользу явных
SSE start/drain этапов; если SSE не используется, эти этапы пропускаются.
Старая скрытая Access→roles связь остаётся историческим предложением,
а не текущей зависимостью реализованного AccessMiddleware.

## Зафиксированные контракты

- Request — вход, Response — синхронный writer; stack wrappers не переносятся
  в другие задачи. HTTP body до 4096 bytes читается до handler/pipeline.
- WS send возвращает WebSocketSendResult; broadcast — queued/rejected/first_error.
  Queued не ACK. До 4 pending jobs/session, payload до 4096 bytes;
  QueueFull пока не закрывает сессию. Общий WS drain/cancel не заявлен.
- SSE включён в core, один worker на запущенный EventSource. Auth/policy до
  open; resolveHeaders→owned preface→reserve→claimExternal→activate.
  Request/Response/Next/application callbacks не сохраняются.
- SSE publish копирует Frame через try-lock в bounded Sessions. Individual
  overflow/stall закрывает свою сессию; GlobalFull не обвиняет клиента.
  Shutdown идёт до async complete; успешный stop нужен до Server::end.
  При false от stop объект и сервер сохраняются для повторного drain.
- Native authenticated WS, HTTPS/WSS, pre-body auth и streaming upload остаются
  нереализованными. SSE plain HTTPD не допускает TLS/send overrides/forced close.
- Wi-Fi, storage mounts, product UI, admission policy S3 и CBOR DTO — приложение.
  Shared frontend в корне; CBOR map используют UTF-8 text keys без реестра ID.

## Документация и проверки

Синхронизированы `agents/architecture.ru.md` / `agents/architecture.md`,
переписано текущее `docs/readme_architecture_ru.md`, дополнен docs/README.md.
Даты пары проверены; изменения сопоставлены по смыслу в обеих версиях.
Исторические ADR/verification reports не переписывались.

Через MCP выполнены layout_guide/get_kinds, get_view(hidden=true), чтение текстов
всех сущностей, get_relations, graph_status и doctor. Финальный render_view
каждой из шести карт вернул **no problems found**, изображения просмотрены.
Doctor: `extractorsOK=true`, `findings=null`. Старые API/SSE формулировки
в текстах сущностей больше не найдены.

Локальные Markdown-ссылки пяти изменённых документов и отсутствие хвостовых
пробелов проверены: PASS. Scoped git diff --check с учётом CRLF: PASS.

Новые builds, flash и сетевые/аппаратные тесты не выполнялись. Числа C3/S3 из
существующих отчётов остаются историческими результатами указанных ревизий.
SSE stop tests не заменяют проверки shutdown ожидающих WS jobs или длительный soak.

## Состояние сохранения

После отдельного явного поручения владельца «Сохранить все шесть карт»
выполнен MCP save для проекта до переименования: **project saved**.
Повторный get_view подтвердил **unsaved=[] во всех шести картах**;
число видимых связей осталось 7 / 5 / 6 / 12 / 8 / 8
(v_layers / v_context / v_tasks / v_flow / v_app / v_access).
Doctor после сохранения: extractorsOK=true, findings=null.
Markdown-файлы записаны в рабочее дерево. Commit/push не выполнялись.

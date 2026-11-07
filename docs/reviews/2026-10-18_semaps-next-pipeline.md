# SeMaps: обновление под реализованный next pipeline

2026-10-18. Неизменяемый снимок после реализации middleware. Обновлены три существующих
представления через штатный MCP SeMaps MCP, без чтения/правки registry/view файлов.
Новый C++ extractor не настраивался: модель авторская, проверена по исходникам.

| Представление | Итог |
|---|---|
| [v_layers](http://127.0.0.1:54381/app/#v_layers) | Новый HTTP auth/access, Route/static table и три фасада вместо decorator/container |
| [v_app](http://127.0.0.1:54381/app/#v_app) | Владение providers/chains/routes, синхронные before/next/after и срок жизни Next |
| [v_access](http://127.0.0.1:54381/app/#v_access) | Реализованный pipeline/FixedBasic и явно будущие роли, storage, sessions, feature split |

Изменённые объекты:

| ID | Что уточнено |
|---|---|
| e_auth | AuthMiddleware + AccessMiddleware, отказ на пустые credentials |
| e_container | Область композиции теперь Chain/makeChain/FixedChain; старый owning container удалён |
| e_server | Route metadata и borrowed static table; body пока читается до pipeline |
| e_app_own | Приложение владеет providers, middleware, chains, route table и данными |
| e_app_register | Связать routes/chains до begin, отдельные static и registration режимы |
| e_app_incoming | Синхронный callback на HTTPD, request-local Context/Principal |
| e_app_contract | Next 0/1, без переноса stack continuation в другую задачу |
| e_access_routes_zone | HTTP route policies реализованы |
| e_access_guards_zone | Выбранный next pipeline реализован |
| e_principal | Реальные поля: authenticated, user_id, permission mask; revision не реализована |
| e_authenticator | AuthMiddleware вызывает borrowed provider |
| e_authorizer | AccessMiddleware плюс fail-closed проверка перед endpoint |
| e_user_store | Реализованный FixedBasicAuthenticator, один пользователь в code config |
| e_roles_catalog | Изменяемые роли/NVS/FS остаются планами |
| e_sessions_ram | RAM login sessions остаются планами |
| e_access_data_zone | Разделён статус готового provider и будущих модулей |
| e_build_choice | Feature split/Kconfig ещё план |
| e_access_phase | Следующий HTTP шаг — проверка новой прошивки на C3; WS auth/SSE впереди |
| v_access.name | Доступ: pipeline и планы хранения |
| r_authorizer_roles_catalog_storage | Старая предложенная связь скрыта на v_access: текущий Access читает Principal |
| r_authenticator_principal_data_flow | Новая authored связь: authentication заполняет Principal |
| r_principal_authorizer_data_flow | Новая authored связь: permission check читает Principal |

Геометрия блоков не менялась. Все три render_view визуально проверены, каждый вернул
no problems found. Владелец явно ответил «Да, сохранить обновления SeMaps»; MCP save
подтвердил project saved. get_view после сохранения: unsaved=[] во всех трёх картах;
visible edges: 6 / 6 / 7. Doctor: findings=null. Проверка сохранённых файлов
`semaps check проект-схем до переименования` завершилась с exit 0, замечаний нет.

Остальные три представления не менялись. Этот отчёт не заменяет hardware tests:
карта не доказывает boot, устойчивость очереди, cleanup при stop и сетевое поведение.

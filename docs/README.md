# Документация NaWServer

- [English README](../Readme.md) / [Русское README](../Readme.ru.md): возможности, переключатели, измерения и ограничения.
- [06.11.2026: Мини / Нормальный / Максимальный на C3/S3, память с процентами и CPU/API p95](readme_profile_benchmarks_ru.md).
- [06.11.2026: основная программа и веб — сроки задач, приоритеты, ядра и задержки HTTP/WS/SSE](readme_async_workload_ru.md).
- [Проверка выпуска 30.10.2026](reviews/2026-10-30_release.md), [происхождение и MIT](reviews/2026-10-30_provenance.md), [сторонние лицензии](../THIRD_PARTY_NOTICES.md).
- [07.11.2026: аудит публичного снимка — секреты, состав и проверки чистой копии](reviews/2026-11-07_publication.md).

Датированные отчёты ниже описывают указанные снимки и конфигурации, а не новую
проверку текущего выпуска. Адреса стенда в опубликованных материалах обезличены.

- [Ключи API: NVM, web/консоль и метки страниц](readme_api_keys_ru.md).
- [03.11.2026: общий engine, ранние middleware, Drop и потоковые тела](readme_20261103_portable_revision_ru.md).
- [Общая передача HTTP/WS/SSE: конфигурация, source/writer/acquire и статистика](readme_unified_transmission_ru.md).
- [02.11.2026: аппаратные проверки общей передачи, CPU/память и открытые ограничения stress](readme_20261102_unified_tx_verification_ru.md).
- [03.11.2026: чистые WS/SSE/idle замеры C3 и повторение duplex-ограничения](readme_20261103_clean_observer_measurements_ru.md).
- [План профилей ресурсов: define, квоты IP/key, формулы соединений и памяти](plans/PLAN_20261103_client-resource-profiles.md).
- [Переносимая CMake-сборка, lwIP и FreeRTOS-профиль RP2350](readme_portable_build_ru.md).
- [SSE непосредственно через общую очередь передачи](readme_sse_shared_transmission_ru.md).

- [02.11.2026: переносимая диагностика TCP listener, восстановление и fault-тесты](readme_20261102_tcp_listener_recovery_ru.md).

- [Распределение RAM: тонкая полоска, вертикальные столбики и реальные категории](readme_memory_distribution_ru.md).

- [27.10.2026: минимум + динамика памяти, опциональный API-резерв, keep-alive и приёмка C6/S3](readme_20261027_memory_manager_keepalive_ru.md).
- [Решение: начальные/динамические буферы и ограниченный keep-alive](adr/ADR_20261027_nawserver_managed-buffer-lifetime-and-keepalive.md).

- [27.10.2026: причины REST dial timeout и WS close, исправление и повторные замеры C6/S3](readme_20261027_tcp_rest_ws_close_ru.md).

- [27.10.2026: настройки памяти, жёсткие лимиты, REST/UI 1–3 клиента и перегрузка](readme_20261027_memory_limits_tests_ru.md).
- [Решение: независимые от демо бюджеты и размещение буферов](adr/ADR_20261027_nawserver_buffer-memory-and-resource-limits.md).
- [Публичные настройки памяти TCP/SSE, ESP-IDF policy и жёсткие лимиты](readme_buffer_memory_ru.md).
- [Контрольный срез 27.10.2026: выводы, JSON, Flash/RAM и нагрузка для сравнения](readme_20261027_json_load_comparison_ru.md).
- [Журнал сборок ESP: конфигурации, Flash, статическая и runtime RAM](readme_build_memory_log_ru.md).
- [Общие SDK IDF 5.5.6 / 6.1 и профили сборки](readme_shared_idf.md).

- [Идея: HMAC / цифровые подписи для аутентификации CBOR-команд](ideas/IDEA_20261025_nawserver_authenticated-cbor-commands.md).
- [Идея: единая матрица CPU/RAM/Flash и нагрузочных проверок одним батником](ideas/IDEA_20261025_nawserver_resource-and-stress-matrix.md).

- [Требования: автономный MCP на контроллере, define-флаги и общий JSON-интерфейс](readme_mcp_json_requirements_ru.md).
- [Реализованный MCP: протокол, JSON, инструменты, демо и проверки](readme_mcp_implementation_ru.md).
- [JSON: выбор bounded/yyjson/RapidJSON, define/Kconfig, память и повторное использование библиотеки](readme_json_backends_ru.md).
- [Ночная проверка JSON, S3 Wi-Fi и нагрузки 26/27 октября](readme_overnight_json_load_ru.md).
- [Сводная матрица JSON: Flash, RAM, скорость и результаты нагрузки](readme_json_matrix_ru.md).
- [Итог ночи 26–27 октября: JSON, сочетания и три четырёхчасовых прогона](readme_json_night_results_ru.md).
- [Ограничения синхронной HTTP/WS отправки под reload-нагрузкой](readme_tcp_load_findings_ru.md).

- [Историческая точка продолжения на25 октября; P5 уже завершён, дальше P6](plans/PLAN_20261024_nawserver_tcp-engine-resume.md).
- [P6: фактическая SDK/adapter подготовка и открытая lwIP close policy](plans/PLAN_20261025_nawserver_idf-tcp-adapter.md).

- [Решение: общий HTTP/WS через TCP и выбор IDF HTTPD](adr/ADR_20261024_nawserver_shared-tcp-http-engine.md).
- [Исследование: границы, копии, память, риски и IDF 5/6](reviews/2026-10-24_shared-tcp-http-engine.md).
- [Спецификация TCP/HTTP/WS/SSE для реализации](plans/PLAN_20261024_nawserver_tcp-engine-contracts.md).
- [Этапы реализации, полная приёмка, нагрузка МК и Linux рядом с ioBroker](plans/PLAN_20261024_nawserver_tcp-engine-implementation.md).

[Текущая реализация и проверки по этапам](readme_tcp_engine_verification_ru.md):
P0–P5 выполнены; [общие TCP HTTP/WS/SSE и композиция](readme_tcp_http_ru.md)
проверены в ограниченном C3 MVP под IDF5.4.0/6.0.3. Полная приёмка P6 и
нагрузочные/длительные проверки P7 остаются открытыми. Исторические результаты
и текущий MVP-профиль в отчёте учитываются отдельно.

- [Переименование проекта и миграция потребителей](readme_nawserver_migration_ru.md).

- [INFO: компактные графики и расширенный паспорт устройства](readme_info_ru.md).

- [Демка ESP32-C6-LCD-1.47: сборка, распиновка и тест LCD](../examples/c6_lcd147/README.md).

- [MIME статических ресурсов, отказы WS и динамическая доступность функций](readme_static_ws_ru.md).
- [Проверка MIME и результатов WS-отправки](readme_static_ws_verification_ru.md).
- [Общий WebSocket: memory, Windows и ESP-IDF, контракт и проверки](readme_ws_adapters_ru.md).

- [Использование и проверки](readme_usage_ru.md).
- [Результаты проверки](readme_verification_ru.md).
- [Решение о границах компонента](adr/ADR_20261018_nawserver_component-boundary.md).
- [Незавершённые проверки](plans/PLAN_20261018_nawserver_acceptance.md).

- [Архитектура: две актуальные карты SeMaps и шесть архивных](readme_architecture_ru.md).
- [Сверка всех шести карт с текущим кодом — 24.10.2026](reviews/2026-10-24_semaps-current-architecture.md).
- [SSE и защита WebSocket](readme_realtime_ru.md).
- [Библиотечный SSE: EventSource и lifecycle](readme_sse_ru.md).
- [Общий SSE: контракты и адаптеры Memory, Windows, IDF](readme_sse_adapters_ru.md).
- [Проверка SSE после включения в библиотеку](readme_sse_library_verification_ru.md).
- [Разбор middleware и вариантов доступа](reviews/2026-10-18_middleware-and-access.md).
- [Предложение: фичи сборки, роли и хранилища](ideas/IDEA_20261018_nawserver_features-and-access.md).

- [Решение: статическая композиция и новый API без legacy](adr/ADR_20261018_nawserver_static-composition-and-clean-api.md).

- [Решение: next pipeline и плоские ресурсы](adr/ADR_20261018-2_nawserver_static-next-pipeline.md).

- [Предложение: три формы Chain и медленные клиенты](ideas/IDEA_20261018_nawserver_chain-facades-and-backpressure.md).

- [Решение: локальные лимиты и отключение медленного клиента](adr/ADR_20261018-3_nawserver_local-device-limits.md).

Актуальные правила — в [AGENTS.md](../AGENTS.md) и [agents/](../agents/architecture.md).
Жанры документации: решения неизменяемы, планы изменяются по мере работы, руководства
и описание реализации обязаны соответствовать коду.

- [Точка продолжения: MCP, архитектура и middleware](plans/PLAN_20261018_nawserver_architecture-resume.md).

- [Реализованный middleware: три формы цепочек и статические маршруты](readme_middleware_ru.md).
- [Проверки нового middleware](readme_middleware_verification_ru.md).
- [Обновление трёх карт под next pipeline](reviews/2026-10-18_semaps-next-pipeline.md).
- [Идея следующего этапа: опциональные HTTPS и WSS](ideas/IDEA_20261018_nawserver_optional-https.md).

- [Тело запроса: raw, своя память, std::array и vector](readme_request_body_ru.md).
- [Проверка body API на C3](readme_request_body_verification_ru.md).

- [Две реализации URL-декодера: Lookup и from_chars](reviews/2026-10-19_url-decoder-implementations.md).
- [Оставшиеся задачи HTTP и WebSocket](plans/PLAN_20261019_nawserver_http-ws-follow-up.md).

- [Четыре URL decoder: контракт, сравнение и реальные замеры C3](readme_url_decode_benchmark_ru.md).
- [Интеграция Lookup/from_chars, binary-safe query/form и проверки C3](readme_url_decode_integration_ru.md).

- [Синхронный Request/Response, DefaultHeaders и профили ответов](readme_http_api_ru.md).
- [Проверки нового HTTP API на компьютере и C3](readme_http_api_verification_ru.md).
- [Решение: синхронный Request/Response и профили](adr/ADR_20261019_nawserver_synchronous-http-api.md).

- [Showcase: TechnicalShell, параллельные WS/SSE и проверки доступа](../examples/showcase/docs/readme_refresh_20261019_ru.md).

- [Showcase на физической C3: WS/SSE одновременно, auth и настоящий UI](../examples/showcase/docs/readme_c3_realtime_verification_ru.md).

- [Отдельное демо Waveshare S3 Touch LCD 7](../examples/touch_lcd7/README.md).

- [Лимиты соединений, резерв и runtime-диагностика](readme_connection_limits_ru.md).
- [Опциональный допуск, резерв API по ключу и подсказка IP для TCP](readme_admission_ru.md).
- [Решение: управляемый отказ при перегрузке](adr/ADR_20261019_nawserver_connection-admission-and-reserve.md).

- [Единый frontend C3/S3](../frontend/README.md), [решение](adr/ADR_20261020_nawserver_shared-demo-frontend.md).
- [Проверка общего frontend](readme_shared_frontend_verification_ru.md).

- [Требования CBOR со строковыми ключами](readme_cbor_requirements_ru.md), [решение](adr/ADR_20261020_nawserver_cbor-text-field-names.md).
- [Проверки CBOR и развёртывание на S3](readme_cbor_verification_ru.md).

- [Рекомендуемая структура и версии окружения, перенос между рабочими местами](readme_development_environment_ru.md).

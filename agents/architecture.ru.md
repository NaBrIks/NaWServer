# Текущая архитектура компонента

Визуализация RAM принадлежит приложению: опциональное memory_usage в summary
сочетает реальные linker-границы данных и SDK heap samples. ESP allocator
буферов отдаёт лёгкие счётчики живых payload по фактическому адресу internal/PSRAM;
глобальной трассировки и знания о демо в публичном ядре нет. Неучтённые выделения
и расходы allocator явно остаются прочей динамикой. Границы, fallback и проверки:
docs/readme_memory_distribution_ru.md.

## Управляемые буферы и постоянный HTTP (2026-10-27)

Переносимый MemoryManager удерживает только заданные начальные блоки, выделяет
остальные фактического размера в пределах слотов/байтов/свободной памяти и сразу
освобождает динамические при возврате. MemoryBlock — move-only lease с явным
сроком жизни менеджера. Механизм используют HTTP metadata/ответы, ленивые тела
запросов, WS arenas при upgrade и SSE frame sets при подключении; ограниченные
ingress и таблицы дескрипторов остаются начальными. Физический резерв HTTP
основного ключа опционален и отличается от квот admission. Центральные readiness
для конкретного размера и причины блокировки позволяют отказать заранее;
начальные блоки доступны для повторного использования и при нехватке heap.
Platform probe/guarded allocation находятся за BufferAllocator; Windows/Linux
не предполагают ESP heap или универсальный порог свободной RAM всей системы.
Response.sendOwned передаёт lease без второй TCP payload-копии; sendImmutable и
обычные копии с безопасным временем жизни сохраняют свои контракты.
Опциональный последовательный HTTP/1.1 keep-alive оставляет между запросами
только socket/idle metadata и заново проверяет admission. Пределы простоя/числа
запросов и явный Connection: close ограничивают соединение. Это заменяет
резервирование всех jobs/arenas на старте, не добавляя pipeline, параллельные
callbacks или глобальное управление выделениями SDK. Scheduler может работать
без Admission. Настройки и доказательства: docs/readme_buffer_memory_ru.md и
docs/readme_20261027_memory_manager_keepalive_ru.md.

## Опциональный допуск и QoS (2026-10-27)

Admission<N> — независимая политика без выделений, со статическими таблицами
клиентов и разрешений. Настроенный основной ключ получает свою квоту; допущенные
общие идентичности сохраняют индивидуальные квоты между запросами до истечения
простоя. Ключ распределяет ресурсы; аутентификация остаётся у приложения.
TCP опционально применяет политику до body: ограниченный ingress, индивидуальные
HTTP jobs и WS arenas. Короткие503/429 и ограниченные шаги I/O сохраняют
последовательные callbacks без повторного входа. Обычный Response копирует
ограниченные данные до возврата; sendImmutable явно заимствует неизменяемое
тело, живущее до остановки сервера. Middleware с той же policy не списывает
ранний TCP допуск повторно; самостоятельно это более поздняя проверка.
Опциональный callback доверенного адреса оставляет один ingress slot для
выбранного приложением IPv4, не выдавая квоту основного ключа и не обходя
аутентификацию. Все ingress/native socket ресурсы конечны: насыщение TCP отличается
от завершённого HTTP-отказа. Компактный TCP адаптер имеет один WS на идентичность;
приложение согласует суммарные пределы WS/SSE/socket. Исключение из сборки —
CONFIG_NAWSERVER_TCP_ADMISSION и host build option. Демо только задают тестовые
конфигурации. Измеренная цена и приёмка: docs/readme_admission_ru.md и
docs/readme_20261027_admission_qos_ru.md.

## Память буферов и пределы ресурсов (2026-10-27)

Публичные TCP HTTP/WS и SSE frames имеют отдельные allocator и жёсткие бюджеты
запрошенных байтов на экземпляр. Общее ядро не знает о LCD, демо и политиках
плат; ESP-IDF предоставляет строгие internal/PSRAM и явно разрешённые
предпочтение/fallback через свой helper и defaults Kconfig. JSON/MCP сохраняет
caller-owned workspace. Стеки, синхронизация и память ОС/SDK учитываются отдельно.
Capability malloc гарантирует лишь выравнивание4 байта: ESP allocator буферов
и caller-owned JSON storage явно обеспечивают max_align_t.
Пределы admission, сообщений и очередей отклоняют превышение до выделения payload.
Обычная REST-автоматизация (1/2/3 последовательных клиента), HTTP-посещение UI
и перегрузка — разные профили. Последующая диагностика TCP добавила IDF TCP_NODELAY
и публичные пределы grace/слива HTTP/WS при закрытии (по умолчанию 250 мс / 8192 Б).
Захват пакетов выявил старое TCP-состояние при повторе исходного порта и WS RST
с непрочитанным входом; короткая обычная матрица теперь проходит, но reload-перегрузка
по-прежнему даёт таймауты. Ни свободная RAM, ни короткий прогон не доказывают soak.
См. docs/readme_buffer_memory_ru.md, docs/readme_20261027_memory_limits_tests_ru.md
и docs/readme_20261027_tcp_rest_ws_close_ru.md.

## Общий TCP-движок (2026-10-25, код и сборки P6; приёмка C3 не завершена)

Владелец выбрал один HTTP/WS wire engine поверх Winsock, Linux sockets и IDF
lwIP BSD sockets с сохранением EspIdfHttpBackend как независимо выбираемого
пути через HTTPD. Сохранить HttpBackend/HttpExchange и общий dispatcher;
переиспользовать существующие SSE encoder/Sessions/bridge. Это заменяет ограничение
«только IDF», но не прикладные границы и не запрет универсальной HAL.
Общий TCP HTTP/WS/SSE и bounded storage реализованы на Windows, isolated Linux и
IDF через конкретный `tcp::IdfTransport` и private FreeRTOS worker. Native HTTPD,
прежние Windows/memory пути сохраняются. Код P6 и SDK-сборки реализованы, но этап
не закрыт: функциональные проверки C3 частичны, browser failures исследуются.
Нагрузочная и длительная приёмка МК остаётся незавершённой в P7. Поэтапные подтверждения находятся
в docs/readme_tcp_engine_verification_ru.md.

Наш код должен избегать лишних копирований и выделений: стабильные ограниченные
хранилища и views, прямая синхронная отправка, явно владеющие данные в очередях.
У каждой существенной копии/аллокации должны быть срок жизни, предел и причина.
Буферы, стеки задач, очереди, сохранённые metadata и Flash dependencies с расходами
в KiB необходимо измерять. Статическое хранилище не обязательно меньше; runtime
лимит очереди не равен размеру зарезервированного хранилища. Сохранять копии,
необходимые для безопасного владения; не обещать полный zero-copy или zero-heap.

Выбор native HTTPD или TCP-only и независимые флаги WS/SSE реализованы в Kconfig
и component CMake. В standalone графе TCP-only нет HTTPD/esp-tls; другие компоненты
приложения могут добавить эти зависимости самостоятельно. На checkpoint до safer-close
для C3 прошли 16 SDK build profiles:
IDF 5.4.0/6.0.3 × HTTPD/TCP × HTTP/WS/SSE/WS+SSE. Native строки
обновлены после SDK6 post-handshake исправления; данные до него сохранены отдельно.
Текущий safer-close MVP пересобран в двух полных TCP-профилях IDF 5.4.0 и 6.0.3
с HTTP+WS+SSE: 2/2 PASS. Вся матрица из 16 профилей после этого изменения не повторялась.
Другие minor/SoC не проверены; языковая база — C++17. Текущий приоритет владельца —
рабочий MVP на C3; Windows/Linux ограничены быстрыми направленными проверками разработки.

Текущий функциональный срез native C3 включает on-device response 89 / loopback 63 /
lifecycle 30 и ограниченные LAN HTTP/WS/SSE проверки. Native IDF5 и IDF6 после post-hook
исправления прошли Go HTTP/WS, допуск трёх SSE-клиентов и остановку двух SSE-клиентов
с WS witness. Это не полный browser gate:
browser failures/connection stalls остаются открытыми и не объявлены исправленными
на основании одного успешного повтора. P6 не закрыт; точные профили и результаты —
в поэтапном отчёте, не в универсальной формулировке «поддерживает все IDF5/6».
Текущие safer-close TCP/C3 сборки на обоих проверенных SDK прошли on-device 89/63/30,
LAN HTTP/WS, response probe из 78 проверок с двумя клиентами, допуск трёх SSE-клиентов
и остановку двух SSE-клиентов с WS witness. Обычный browser-прогон SDK5 удерживал один
WS и два SSE-потока на 20 итерациях: каждый SSE получил все 20 событий, совпали 40 WS
echo, drain завершил ровно два SSE-потока. Аналогичный обычный steady-прогон SDK6
тоже прошёл: 20 health-проверок, 20 text и 20 binary WS echo, по 20 событий каждому
SSE-потоку. SSE begun/completed достигли ровно 2/2, active/queued — нуля после одной
попытки cleanup; сам EventSource продолжает работать. Более ранний повторный
coexistence-прогон полного UI завершился WS timeout и остаётся отдельным результатом.
Это подтверждение ограниченного MVP C3, не приёмка четырёх клиентов, полного P6,
регрессии полного UI или длительной работы.
Приёмка включает deterministic partial-I/O, реальные браузеры, смешанную нагрузку
2–4 клиентов под возможности МК, bounded overload/recovery и soak с измерением
памяти/задержек. Linux рядом с рабочими службами проверяется отдельным ограниченным
процессом; разрушительные stress/fuzz/OOM сценарии выполняются на отдельном стенде.

Перед реализацией прочитать docs/adr/ADR_20261024_nawserver_shared-tcp-http-engine.md,
docs/reviews/2026-10-24_shared-tcp-http-engine.md,
docs/plans/PLAN_20261024_nawserver_tcp-engine-contracts.md и
docs/plans/PLAN_20261024_nawserver_tcp-engine-implementation.md.
В исследовании записан исторический host-результат 17/18 с успешным отдельным повтором SSE,
а не чистая приёмка. Предложенные runtime детали и числовые бюджеты — инженерные
рекомендации; готовность поддержки должна следовать измеренным результатам.

## Общий HTTP-путь и датированные checkpoints

Общий HTTP-путь использует явный контракт `tcp::Transport`, concrete adapters
Winsock/Linux/lwIP, фиксированные слоты с поколениями
и владением, scripted transport tests. Bounded parser/writer без allocations
передают данные тому же HTTP dispatcher через TcpHttpBackend и последовательный
worker. P4 использует один reusable HTTP body workspace и проверенный borrowed
preparedBody view; другие backends сохраняют owned RequestBody fallback. OS worker glue приватен.
Реализованы общие WS handshake/frame codec и SSE encoder/Sessions/bridge,
сохраняя исходные handlers и синхронный HTTP pipeline. Датированный результат P2:
28/28 host tests и12/12 browser fetch с повтором; текущие подтверждения P6
записываются отдельно в docs/readme_tcp_engine_verification_ru.md.
Запросы startup allocations в compiler-only профиле C3 P4 составляют13395 B для
HTTP-only или29879 B с таблицей на четыре WS и одним общим TX pool4x4096 B,
включая array cookies, до allocator/worker/maps overhead. Queued и active WS frames
владеют move-only pool leases. Busy и GlobalFull сохраняют соединение; individual
QueueFull запрашивает close1013 через I/O owner, сохраняя partial frame.
HTTP-only не выделяет backing pool. Эти размеры и compiler frames не являются измерением
runtime heap/stack SDK и не заменяют отдельные функциональные результаты C3.
Использование, выбор backend/feature flags и явные start/stop:
docs/readme_tcp_http_ru.md. SDK graph/link проверяются отдельно от host source list.
При ambiguous retained close cleanup удерживается без слепого
retry, который мог бы закрыть переиспользованный descriptor.

P5 прошёл native isolated Linux GCC/Clang и ASan/UBSan41/41, Windows44/44,
отдельный TSan concurrency/lifecycle corpus и два часовых wire fuzz jobs.
В той исторической P5-ревизии один shutdownWrite предшествовал bounded TCP drain.
Текущий HTTP/WS cleanup отбрасывает до 4096 B/50 ms до единственного close владельцем,
без отдельного half-close. HTTP использует прежний scratch 256 байт, WS — прежний RX
после полного Close frame, без дальнейших callbacks. SSE непосредственно вызывает
registry close с прежними retained-close/retry правилами и без нового RX drain.
Transport::shutdownWrite остаётся настоящим half-close API для явных вызовов;
cleanup его не вызывает. Safer-close не добавляет буферов, retained storage или allocations.
Deadline
ограничивает I/O, а не обещает hard real-time reclamation при занятом owner.
C3 connection sizeof1008 B, это compiler-only, не hardware heap/high-water.
SseTest diagnostic app сохраняет owned metadata при initial Busy и публикует
из явного owner poll; core Busy/queue policy не менялась. Его C3 object+592 B,
poll own frame1024 B требуют учёта в sample budget, не во всех приложениях.
Текущий scope/ограничения/воспроизводимые evidence находятся в поэтапном отчёте.

Владелец разрешил узкую границу HTTP backend для тестов в памяти, Windows и IDF.
HttpBackend/HttpExchange/HttpRequestHandler определены в http_backend.h;
HttpServer в http_server.h заимствует backend и статическую таблицу маршрутов.
Общий dispatchHttp сохраняет порядок body -> query/form -> handlers -> pipeline
-> endpoint -> Response. Request и Route не включают SDK headers;
HttpMethod/HttpError принадлежат компоненту. ResponseTransport остаётся общей
границей синхронной отправки. Это не универсальный HAL/RTOS/network abstraction.

Host library собирается из host/CMakeLists.txt без SDK stubs. MemoryHttpBackend
подаёт разобранные запросы в тот же dispatcher и моделирует ошибки чтения/отправки.
WindowsHttpBackend использует Winsock, loopback, последовательные callbacks,
ограниченный HTTP/1.0/1.1, Content-Length и Connection: close. Windows и memory
поддерживают общий WebSocket endpoint и SSE; TLS на host не реализован.
Страница frontend/http-test/ использует отдельные HTML/CSS/JS;
она проверяет backend настоящими fetch, без копирования существующего demo UI.

Нативный Server остаётся HTTPD facade, владеет EspIdfHttpBackend и сохраняет проверку/диспетчеризацию
маршрутов. backends/esp_idf.h и src/backends/esp_idf.cpp владеют конфигурацией,
запуском HTTPD, регистрацией нативных WS, контекстами callbacks и остановкой.
Обычный HTTP проходит через общий dispatcher и EspIdfHttpExchange. SDK-доступ
вынесен в backends/esp_idf_request.h; SSE использует это расширение. WS использует
независимый от платформы WebSocketExchange. HttpServer позволяет совместить HTTP
и WS маршруты с native IDF, memory, Windows и общим TCP backend в пределах
реализованных платформ каждого backend.
Неудачный запуск с неудачным откатом возвращает CleanupRequired через общий API:
HttpServer не запущен, но сохраняет cleanupRequired до успешного end.
Нельзя освобождать handle/контексты/зависимости после неуспешной остановки.
Нативный Server.running отражает владение handle, включая неудачный откат запуска.
Настройки native parser/socket waits остаются у SDK; неподдержанные отличающиеся
от defaults host-настройки отклоняются. Новая политика deadline здесь не добавлена.
Host lifecycle стенды компилируют canonical backend/facade sources с управляемыми
SDK stubs при WS=0/1 и SDK6 post-hook model. Они не проверяют настоящие SDK tasks
или поведение устройства. Историческая HTTPD-ревизия от2026-10-24:
89 response, 63 loopback и 27 lifecycle проверок прошли; LAN smoke и 198 HTTP
response проверок прошли. Общий мини-UI прошёл 14 browser проверок на C3.
Сборки запускаются в фоне. Это не подтверждает другие МК/устройства.
Новые host/browser результаты описаны в docs/readme_http_api_verification_ru.md;
нижеследующие аппаратные результаты
остаются историческими. Мелкие утилиты/декодеры/CBOR этим этапом не изменяются.

## Размещение исходников (сверено 2026-10-25)

Публичные заголовки находятся в include/NaWServer и подключаются как <NaWServer/...>.
Основные точки входа: server.h, request.h, response.h, websocket.h и sse.h.
Реализация разделена между src/backends, src/http, src/auth, src/routing, src/websocket и src/sse.
CMake экспортирует только include; src — закрытый каталог заголовков.
Заголовки реализации SSE, необходимые для конкретного класса со статическим владением,
находятся в include/NaWServer/detail/sse. Это детали реализации, не API приложения.
Host-тесты могут явно подключать detail-заголовки для проверки внутреннего устройства.
Исторические отчёты и хеши сохраняют пути, использованные в проверенной ревизии.

Общие закрытые вспомогательные функции протокола находятся в `src/detail/ascii.h`
(сравнение ASCII без зависимости от локали) и `src/http/header_validation.h`
(синтаксис имён и значений HTTP-заголовков). Response и SSE используют одинаковые
проверки синтаксиса, но сохраняют собственные правила зарезервированных заголовков,
дубликатов, времени жизни и вместимости. Сравнение схемы Basic и media type формы
использует тот же ASCII-код. Эти заголовки не относятся к API приложения.

Приложение МК выбирает native ESP-IDF HTTPD либо общий TCP; язык — C++17.
Приложение владеет инициализацией сети, монтированием хранилищ, продуктовыми данными,
обработчиками, провайдерами, middleware, цепочками и таблицами маршрутов. HttpServer
заимствует явно переданный backend; native-фасад Server владеет EspIdfHttpBackend
и его экземпляром HTTPD. Конструкторы связывают зависимости и не запускают ресурсы;
`begin()` запускает ресурсы времени выполнения. Инициализируйте esp_netif до запуска
IdfTransport. Остановите производителей и завершите drain EventSource до server.end(),
затем остановите транспорт до уничтожения заимствованных зависимостей. Worker glue
приватен и не является публичным task/clock HAL. Нельзя останавливать сервер из HTTP
callback. Копирование сервера запрещено.

## HTTP-маршрутизация и middleware

- `pipeline.h/.cpp`: синхронный исполнитель, независимый от транспорта; отдельный
  Context для каждого запроса, Principal, фиксированные маски разрешений и AccessPolicy
  маршрута. `Chain<T...>`, `makeChain(instanceRefs...)` и `FixedChain<N>` владеют
  фиксированными массивами невладеющих MiddlewareRef и используют один исполнитель.
  Само выполнение не выделяет память в куче.
- `handler_middleware.h/.cpp`: контракт провайдера AuthenticatorRef, AuthMiddleware,
  AccessMiddleware и FixedBasicAuthenticator для одного пользователя с заимствованными
  учётными данными из кода. Провайдеры предоставляют authenticate(request, principal)
  и challenge() (заимствованное значение WWW-Authenticate или nullptr). Route не
  предполагает именно Basic; Context сбрасывает данные challenge вместе с Principal
  при каждом запуске. NVS, файловое хранилище пользователей и динамический каталог
  ролей не реализованы.
- `route.h/.cpp`: точное совпадение HTTP-метода и пути, заимствованные endpoint/pipeline
  и копия политики. Проверки разрешений запрещают доступ при неуспехе до конечного
  обработчика, даже без явного AccessMiddleware. Публичный маршрут может иметь пустую
  цепочку. Идентификация сбрасывается при каждом запуске.
- `Next` — некопируемое продолжение на стеке, действительное только во время вызова
  middleware и допускающее не более одного вызова. Это не задача, не корутина и не
  отложенный HTTP API. Повторный вызов не запускает нижележащую цепочку снова и
  устанавливает сохраняющийся результат InvalidNext.
- Route преобразует unauthorized/forbidden в 401/403, отсутствие ответа или ошибку —
  в 500. Response учитывает состояние и ошибки отправки и запрещает повторную
  отправку через обёртку. Ошибка после ответа закрывает соединение через диспетчер;
  уже отправленные байты вернуть нельзя. Прямые отправки HTTPD обходят этот учёт.
- `Server` принимает заимствованную статическую таблицу указателей HandlerBase либо
  отдельные регистрации addHandler до запуска. Смешивать способы нельзя. Изменять
  маршруты во время работы нельзя. В native backend остаются выделения памяти
  при запуске для URI и контекстов HTTPD.
- `HandlerBase` остаётся интерфейсом обработчика/транспорта. Удалены старое
  перенаправление через декораторы, владеющий WebServerContainer и неиспользуемые
  callback для body/upload/trivial.

`src/http/handler_static.*` сохраняет простые обработчики заимствованной памяти,
текста, бинарных данных и gzip. Middleware для них не обязателен. MIME задаётся явно:
по умолчанию text/html для текста и application/octet-stream для бинарных данных.
Выдача файлов/источников отложена; монтированием файловой системы владеет приложение.
Поля query/form готовятся до вызова обработчика парсером с явными длинами. Некорректные
escape-последовательности отклоняют весь запрос с 400. Два выбираемых при компиляции
backend процентного декодирования имеют общий контракт: Lookup (по умолчанию) и
std::from_chars. Карты полей владеют декодированными строками, включая встроенный NUL;
библиотека не обещает отсутствие кучи. Ключи query/form точные и чувствительны к
регистру; при дубликатах побеждает первый, form перекрывает query, частичные результаты
никогда не выдаются наружу.

RequestBody либо владеет std::vector<uint8_t>, либо заимствует неизменяемый ByteView
до возврата callback. Общий TCP читает в один стабильный переиспользуемый буфер тела:
один раз копирует уже принятый префикс, затем принимает остаток по конечным смещениям.
Диспетчер проверяет длину и указатель preparedBody до заимствования; некорректная
capability отклоняется. Другие backends сохраняют владеющий путь setBody()/readBody().
body() в обоих случаях возвращает заимствованное неизменяемое представление.
copyBodyTo поддерживает указатель/вместимость, C-массив, std::array и
std::vector. При нехватке места фиксированное назначение возвращает ошибку без записи;
vector может выделять память и повторно использует вместимость. Кодек полезной нагрузки
в этот API не входит. Исходные данные и представления действительны до конца callback;
копиями владеет приложение. query/form/parameter возвращают заимствованные string_view
до уничтожения запроса. Подготовка query/form заимствует исходные представления и
создаёт декодированные поля. Разбирается только тело application/x-www-form-urlencoded
(media type без учёта регистра, параметры допустимы). Остальные тела остаются исходными,
даже если содержат '%' или NUL. path()/rawQuery() — исходные представления URI;
query/form/parameter дают декодированные поля. См. docs/readme_url_decode_integration_ru.md.
HTTP-тело по-прежнему полностью читается до обработчика, предел — 4096 байт.
Аутентификация до чтения тела и потоковая загрузка на этом этапе не реализованы.
Basic сравнивает полные учётные данные; пустое имя или пароль запрещает доступ.
HTTP Basic не предоставляет TLS. Тесты используют публичные тестовые значения,
никогда не секреты устройства.

## WebSocket и выполнение

Native WS отделён от HTTP Route. Публичный WS работает в базовой реализации;
аутентифицированный WS не поддерживается, поскольку HTTPD может выполнить upgrade
до callback приложения. Старой auth-обёртки/контейнера для WS нет. Нельзя утверждать,
что HTTP pipeline авторизует native WS upgrade.

В native IDF backend HTTP callback, WS callback и задания отправки из очереди
выполняются в одной задаче HTTPD. Производители данных приложения и callback ESP-событий работают в других задачах.
Ожидание сокета может блокировать HTTPD; слово async в имени native-отправки не
гарантирует неблокирующий сетевой ввод-вывод. Вытеснение FreeRTOS позволяет работать
другим готовым задачам, но не другому callback в занятой задаче HTTPD. next() этого
не меняет.

WebSocketEndpoint/WebSocketClient и websocket_backend.h не включают заголовки SDK.
Общий endpoint отвечает за допуск, callbacks и broadcast; WebSocketSession владеет
токеном отправки с копированием, WebSocketExchange заимствует данные одного callback.
IDF сохраняет native handshake/frame I/O/HTTPD queue в esp_idf_websocket.cpp.
SDK6 native WS требует `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`: opening
привязан к post-handshake callback, обычный handler остаётся для frames. Без флага
native6 WS build отклоняется. Проверенный SDK5.4.0 этот флаг не использует.
Прежний WindowsHttpBackend использует Winsock и платформенные SHA-1/Base64 с одним select loop;
memory передаёт готовые сообщения и моделирует ограниченную очередь, без wire parser.
Контракт и результаты проверок — в docs/readme_ws_adapters_ru.md.

Общий TCP backend использует независимый от ОС handshake/frame codec и один
последовательный HTTP/WS worker. Допуск WS готовит session/client/container node
до101; только полный101 разрешает opening dispatch через исходный HandlerBase
и commit endpoint. Метаданные сохраняются один раз при допуске: фактические байты
raw head и индекс фактического количества заголовков. Request/Response/client/data
и их views заимствуются только на callback. TCP defaults: четыре WS-клиента
на backend, четыре сообщения в очереди или отправке на клиента,4096 байт на сообщение.
Реализован один общий TX pool 4×4096 B (16 KiB) на backend для всех четырёх WS,
а не отдельное payload storage на каждого клиента. Queued и active frames владеют
move-only pool leases; до completion/abort блок не освобождается. Heap fallback
при заполнении pool отсутствует. Busy/GlobalFull сохраняют соединение, individual
QueueFull запрещает новые sends и запрашивает close1013 через I/O owner, сохраняя
частично отправленный frame. Пределы heap-очередей P3 остались историческими.
Абсолютный срок отправки начинается до ожидания writable. Аутентифицированный
WS по-прежнему не поддерживается; HTTP Route не авторизует этот отдельный upgrade.

В native IDF лимит входа WS по умолчанию —4096 байт. Отдельные контексты URI определяют потоки, поскольку
IDF сбрасывает URI для следующих кадров. Производители копируют данные в обычную кучу
и ставят задания в очередь HTTPD. Токены сессии отбрасывают задания после закрытия,
даже если файловый дескриптор сокета используется повторно. Указатели client/data
заимствуются только на время callback. По умолчанию допускаются четыре клиента
на endpoint и четыре pending-сообщения на сессию; эти границы задаёт WebSocketLimits.
Отправка клиенту возвращает WebSocketSendResult; рассылка возвращает числа принятых
в очередь и отклонённых сообщений и первую ошибку, без отдельного хранилища на клиента.
Queued означает допуск в очередь, не доставку. Этот native queue path не получает
TCP pool/close-on-overflow policy автоматически. IDF stop запрещает новые задания и опустошает очередь
до пяти секунд перед httpd_stop; отказ сохраняет backend для повторной остановки.
Три цикла перезапуска C3 и host queue/error paths прошли; остановка под непрерывной
нагрузкой производителя и длительная работа не проверены. Приложение координирует
производителей и остановку.

## Конфигурация и подтверждения

Выбранный продуктовый сценарий — локальный контроллер примерно на 2–4 пользователей
или устройства, несколько сокетов браузера, явные ограничения ресурсов и закрытие
сессии при зависании или индивидуальном переполнении. У SSE уже реализованы свои
лимиты и сроки; общий TCP имеет bounded waits, HTTP exchange deadline и WS output
deadline. В общем TCP реализованы close-on-individual-overflow и общий 16 KiB TX pool.
Kconfig выбирает native HTTPD (default) либо TCP и отдельно WS/SSE; отключённый
SSE не компилирует encoder/queue/adapter. Native WS зависит от SDK HTTPD_WS_SUPPORT,
а в SDK6 также от явно включённого post-handshake flag; TCP WS от HTTPD не зависит.
Результаты допуска WS реализованы; варианты поведения и границы проверок описаны в
docs/readme_static_ws_ru.md и docs/readme_tcp_http_ru.md. Не развивать
систему QoS, replay или объединения сообщений для медленных клиентов.

`examples/minimal` показывает публичные HTTP/WS и закрытый HTTP-маршрут через makeChain
со статической композицией. `examples/compile_check` создаёт все три формы цепочек
без необходимости интегрировать Wi-Fi. `examples/sse` показывает явный запуск
EventSource и авторизованное открытие через HTTP. Исходники UI демо общие в корне
`frontend/`; проекты добавляют профили/расширения вместо копий frontend/src.

Учитывать сокеты, а не только browser tabs: owner page сохраняется во время
приёмки и может занимать несколько соединений. Native HTTPD по умолчанию сохраняет
обычные HTTP connections, общий TCP закрывает их после одного request. Native SSE
completion освобождает async lease раньше последующего удаления fd задачей HTTPD;
activeSSE не равен общему числу SDK sessions. Эти различия не объявляют причиной
конкретного timeout без соответствующего наблюдения.

Новые и изменяемые комментарии к исходному коду писать на английском. Это не
поручение переводить несвязанные старые комментарии или переформатировать код.

Шесть видов SeMaps описывают слои, владение, контексты исполнения, потоки памяти
WS/SSE, жизненный цикл приложения и HTTP/SSE доступ. Ревизия 2026-10-24 включает
Request/Response, результаты допуска WS и библиотечные SSE worker/handoff/drain.
Это авторские модели, не извлечённый C++ граф: graph_status не содержит extractors.
Проверки и состояние live-модели/сохранения — в docs/readme_architecture_ru.md и
датированном отчёте по ссылке оттуда. Save/discard по-прежнему требует явной команды владельца.
Текущее использование pipeline: docs/readme_middleware_ru.md. Новые проверки описаны
в docs/readme_middleware_verification_ru.md; исторические базовые результаты — в
 docs/readme_verification_ru.md. Следующая ревизия raw/copy body прошита на новой
разрешённой C3: прошли 13 loopback-проверок и сетевой smoke HTTP/auth/body/WS,
включая повтор с закрытой USB-консолью. См. docs/readme_request_body_verification_ru.md.
Эти короткие проверки не подтверждают нагрузочную/длительную работу или другие SoC.

HTTPS/WSS — необязательная функция следующего этапа, не реализована. См.
docs/ideas/IDEA_20261018_nawserver_optional-https.md. Будущая обработка сроков передачи
должна учитывать, что esp_https_server владеет переопределениями send/receive/pending,
транспортными контекстами и open_fn; нельзя занимать эти hooks, предполагая только
незашифрованные сокеты.

## Синхронный HTTP API

Request доступен только для чтения, Response — единственный синхронный писатель
обмена. Диспетчер создаёт оба объекта на стеке. EndpointRef поддерживает
SendResult(Request&, Response&), SendResult(Response&) и Result(Context&) с доступом
к middleware, используя тот же исполнитель pipeline и проверки доступа. Context
теперь содержит response. Общего отложенного HTTP API нет. Response владеет
ограниченным изменяемым хранилищем заголовков; статические DefaultHeaders и
ResponseOptions — заимствованная/общая неизменяемая конфигурация. Тела заимствуются
до возврата синхронной отправки. TextBuffer заменяет прежний владеющий поток ответа;
native HTTPD/lwIP и карты параметров запроса по-прежнему могут выделять память.
Миграция и ограничения: docs/readme_http_api_ru.md и соответствующий отчёт проверок.
Проверкой Basic-токена владеет провайдер; у Request нет методов auth или отправки.
WebSocketEndpoint/WebSocketClient используют транспортный контракт копирования в очередь.

## Библиотечный SSE

Публичный sse.h предоставляет независимый от SDK NaWServer::sse::EventSource,
заимствующий явно переданный sse::Backend из sse_backend.h. Общие encoder/Sessions/bridge
живут в src/sse и detail headers; tools/sse только тестирует/подключает их. IdfAdapter
сохраняет native async HTTPD ownership и FreeRTOS worker, явно подключается через
backends/esp_idf_sse.h. MemoryAdapter использует ручное время и bounded capture;
WindowsAdapter — один worker с неблокирующими Winsock streams. TcpAdapter из
backends/tcp_sse.h заимствует явно переданный TcpHttpBackend identity и также
использует общий QueuedAdapter/Sessions. Host HTTP exchanges предоставляют owned SSE Streams:
rollback сохраняет обычный callback, activation передаёт владение socket.
Request/Response/Next не удерживаются. Исходники компонента и esp_timer перечислены
явно. Конструкторы ничего не запускают; start
создаёт один worker с ограниченными ресурсами. Авторизованные обработчики Route
вызывают open(Request&,Response&); издатели копируют Frame в ограниченные очереди
через try-lock, не ожидая сеть. Queued — не доставка и не ACK.
Порядок resolveHeaders -> owned preface -> reserve -> claimExternal -> activate
управляет допуском первых байтов. Откат в пределах области видимости возвращает
незахваченный запрос в callback; ошибки External устанавливают атомарный флаг abort.
Request/Response/Next/callback не переживают передачу владения. Backend reserve/activate
скрыты из обычного API EventSource.

Для TCP reserve не выполняет native I/O или close; rollback сохраняет HTTP-владение.
Activation подтверждает connection lease, но SSE worker не может отправлять или
закрывать до возврата HTTP callback. Ожидающее закрытие подтверждается только после
освобождения владения underlying lease; retained native close failure сохраняет
активную сессию и неуспешный drain. Остановить производителей, выполнить drain
EventSource, затем end HTTP и stop transport. Только TcpAdapter включает restart
после успешного drain, сохраняя generations: старые SSE tokens остаются Closed.
Поведение restart прежних адаптеров этим не меняется. EventSource можно запустить
до HttpServer::begin; конструкторы ничего не запускают.

Лимиты по умолчанию: 4 слота, 3 ожидающих сообщения на клиента, 768 закодированных
байт на Frame, 2304 байта в очереди клиента, 8192 всего. Heartbeat при простое и
зависание/срок события различаются. Индивидуальное переполнение/зависание закрывает
свою сессию; общий отказ не считается виной клиента. Generation/serial отклоняют
устаревшие шаги транспорта. Остановить издателей и успешно завершить stop/drain
EventSource до server.end. Если stop(timeout_ms) вернул false, сохранить EventSource
и сервер и повторить drain. До уничтожения заимствованных владельцев проверить
esp_err_t от Server::end; HttpServer::end возвращает HttpError.
Windows и общий TCP HTTP end отклоняют остановку при активных SSE registrations, позволяя
владельцу выполнить drain и повторить end. Native IdfAdapter использует plain
HTTPD в проверенной матрице 5.4.0/6.0.3; TCP/IDF использует отдельный TcpAdapter. TLS,
переопределения отправки и внешнее принудительное
закрытие активных async-запросов не поддерживаются. Ограничения WS выше не меняются.
IDF SSE worker ждёт минимум один RTOS tick: перевод1 ms в0 ticks при100 Hz
вытеснял бы задачи меньшего приоритета. См. docs/readme_sse_ru.md и
docs/readme_sse_adapters_ru.md; старые отчёты остаются историческими.

## Допуск соединений (приложение S3)

Server принимает ServerOptions::max_connections до begin(); значение по умолчанию 7
не изменено. ResponseOptions::connection выбирает ConnectionPolicy::Close через
типизированную настройку; произвольные заголовки Connection по-прежнему запрещены.
Это устанавливает заголовок ответа, а не безусловно закрывает native-сокет для любого
транспорта. Пример S3 явно закрывает свою native-сессию отказа после отправки.
Политика допуска, резерв TCP, лимиты WS/SSE, статистика и поясняющий 503 относятся
к examples/touch_lcd7/main/network.*, а не к компоненту. Пул SDK — отдельный предел
времени компиляции. См. docs/readme_connection_limits_ru.md и соответствующий ADR
20261019 connection-admission-and-reserve. Аппаратные результаты относятся только к S3.

## Прикладной протокол и frontend

HTTP/WS передают байты и не навязывают кодек полезной нагрузки. В прикладном CBOR
демо map содержат непосредственно UTF-8 имена полей, нативные CBOR значения и
ограниченный декодер; неизвестные валидные имена не требуют регистрации. Числовой
реестр ключей/fallback в этой архитектуре не нужен. Проверки команд принадлежат
endpoint. SSE демо передаёт текстовый JSON; сам EventSource задаёт framing, не JSON
кодек. См. docs/readme_cbor_requirements_ru.md и docs/readme_cbor_verification_ru.md.
Корневой frontend/ — общий UI демо; расширения LCD/продукта, политика допуска сети
и модели данных остаются в приложении. Существующие отчёты C3/S3 — исторические
подтверждения указанных ревизий, не новые сборки или аппаратные проверки.

## Карта слоёв TCP и IDF HTTPD (28.10.2026)

Сравнительная схема реализации, границы подмены и принадлежность общих ресурсов приведены в [архитектуре NaWServer](../docs/readme_architecture_ru.md). `HttpServer` использует `HttpBackend`; собственный путь TCP — `TcpHttpBackend` → `tcp::IdfTransport` → сокеты lwIP, а IDF-путь — `EspIdfHttpBackend` → `esp_http_server` → сокеты lwIP. `Server` является фасадом именно IDF HTTPD. Транспорт TCP заменяется независимо от backend; HTTPD его не использует. Обе ветви используют сеть и FreeRTOS платформы, а их WS/SSE адаптеры и лимиты не гарантированно взаимозаменяемы.

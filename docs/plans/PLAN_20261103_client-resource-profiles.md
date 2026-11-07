# Профили ресурсов и необязательные квоты клиентов

Дата: 03.11.2026. Статус: требования и план; новые механизмы ещё не внедрены.
Последнее решение владельца: **вся клиентская политика квот — в middleware**.
Квоты IP/page/user/API key в TCP accept/transport не добавлять.
Эта редакция заменяет предыдущие варианты плана с TCP admission gate.

## Граница архитектуры

```
TCP accept -> ограниченное чтение HTTP headers
           -> ранняя middleware chain: identity / optional auth / quotas
           -> подготовка body -> обычный route pipeline -> endpoint
```

TCP/backend отвечает за I/O, глобальную физическую ёмкость, bounded headers,
таймауты, владение handles и выполнение решения закрыть соединение. Он не
считает клиентские IP/page/key квоты и не выбирает политику привилегий.
Глобальный max_connections и бюджеты памяти остаются: это физические пределы
сервера, а не раздача квот клиентам. Без свободного native socket middleware
вызвать невозможно; абсолютную гарантию API при исчерпанном TCP не обещать.

Квоты, идентичности, пользователи и ресурсные профили находятся в middleware/
policy. Backend вызывает общий header-stage hook, не содержит собственного
policy.acquire и не дублирует классификацию. Один результат допуска передаётся
дальше, без двойного списания/повторной проверки credentials. Общий middleware
executor переиспользуется; второй parser/параллельная система квот не нужны.

## Что сейчас существует

- Admission — необязательная policy с bounded tables, HTTP/WS/SSE permits,
  одним reserved_key и обычными identities. Сама policy не требует TCP.
- AdmissionMiddleware вызывает policy в route pipeline. Эта стадия сейчас
  поздняя: body/workspace могут уже существовать.
- Старый optional QosHttp вызывает policy.acquire до body. Это существующая
  реализация, а не уже внедрённая новая IP/page/API схема. В новой архитектуре
  её policy-решения мигрируют в middleware; managed buffers/keep-alive сохраняются.
- Principal {authenticated, user_id, permissions}, AuthenticatorRef,
  AuthMiddleware и FixedBasicAuthenticator уже существуют. Последний читает
  Authorization; аутентификация автоматически с клиентской квотой не связана.
- ExchangeDisposition::CloseConnection уже есть, но полного публичного
  middleware Drop и гарантированного no-flush abort path ещё нет.
- Новые IP/page/NVM API-key квоты и профили из этого плана не реализованы.

## Идентичности страниц, пользователей и API

Каждая экземплярная страница генерирует случайный page_id; две вкладки должны
получать разные метки. Общий frontend client добавляет её к HTTP/WS/SSE,
без дублирования кода по компонентам. Метка не является authentication.
Произвольная смена меток не обходит общий предел middleware по IP.

Для fetch возможен заголовок; browser WebSocket/EventSource не дают произвольных
headers, поэтому несекретный page_id можно передавать в URL. Первый HTML и
скрипт запрашиваются до генерации page_id. Для них нужны middleware правила
anonymous/static requests и общие ресурсные пределы. Не добавлять уникальный
page_id ко всем static asset URLs без необходимости и не ломать общий cache.

Подсчитываются активные страничные сессии; молчащую открытую вкладку невозможно
точно наблюдать без heartbeat. Удаление сессий не зависит исключительно от
уведомления о закрытии: нужны release и ограниченный lease timeout. Проверить
перезагрузку/клонирование вкладки, отсутствие вечных записей и двойного подсчёта.

При включённой аутентификации можно переиспользовать проверенный user_id.
Режимы IP, user, IP+user выбирает профиль. Один user на разных устройствах
объединяется в режиме user и разделяется в IP+user. API/MCP запрос сам по себе
не создаёт нового пользователя. User-Agent/Accept/URL не подтверждают привилегии.

Auth header-only проверка должна выполняться в ранней middleware стадии до
body и использовать существующий provider. Результат относится к конкретному
provider и запросу; поздняя chain не проверяет его повторно. Не сохранять
пароль/Authorization как quota key. Public route не становится требующим логин
только из-за включённого auth module. Invalid credentials не дают проверенную
идентичность. WS проверять до 101, SSE — до streaming 200; обычная route auth
chain сейчас сама по себе WS не защищает.

## Постоянные API-ключи и резерв

API-key — отдельный постоянный секретный токен. Владелец предлагает генерацию
через консоль и web; обе используют одну app-owned операцию и provider.
Число слотов ограничено прошивкой, NVM хранит приложение, библиотека сама
FS/NVS не монтирует. Предложение: хранить hash+metadata, показывать оригинал
при генерации; необходимость повторной выдачи оригинала определить до реализации.
Отзыв/сохранение применяют storage contract платы. Выключенная authentication
не должна случайно открывать удалённое управление ключами.

ioBroker может использовать существующие credentials и дополнительный
X-Client-Key для protected resource profile. Ключ квоты не отменяет permissions
маршрута; связь user/provider с разрешённым reserved profile необязательная,
но явная. При auth=0 проверка reserve key остаётся отдельной QoS/policy опцией,
не считается проверенной пользовательской учётной записью.

Решение владельца: четыре ключа с гарантией одного соединения требуют четырёх
защищённых permits и согласованных buffers/TX resources. Общий пул на несколько
ключей даёт только групповую гарантию. Страницы без специального API-key
пользуются обычными middleware квотами.

Middleware отклоняет обычные identities, чтобы они не занимали protected permits.
На TCP accept лимит обычных IP/страниц не применяется. Новый запрос должен
дойти до ограниченного header-stage; там middleware определяет его профиль.
Это гарантии распределения ресурсов после классификации, не обещание сетевой
доставки или времени выполнения. Ожидание API/WS за длинным HTTP callback
остаётся отдельной задачей.

## Быстрый отказ: Reply и Drop

Ранняя middleware возвращает одно из явных решений (предлагаемый контракт):

- Continue: продолжить chain; только после разрешения готовить/читать body.
- Reply: короткий 400/401/403/429/503 по причине, при необходимости Retry-After;
  ограниченные headers/body/TX и deadline для завершения ответа/закрытия.
- Drop: прекратить обработку без HTTP-ответа, без body, форматирования ошибки,
  fallback 404/500 и ожидания graceful отправки. Backend отменяет TX этого
  соединения и закрывает принадлежащий ему handle.

Drop — узкая transport-independent capability, не доступ middleware к fd,
raw socket, SDK или чужому соединению. Исполняется владельцем I/O после
возврата policy; generation/ownership сохраняются. Не закрывать native handle
из произвольного callback параллельно write, не завершать весь server.
При активном native write память нельзя освободить из-под writer: отмена
должна соблюдать bounded ownership handoff. Для раннего Drop body ещё нет.

Обычный QueuedTransport.close сейчас может flush pending TX с таймаутом;
его недостаточно просто вызвать и назвать быстрым Drop. Отдельно предусмотреть
no-flush cancellation и backend close/abort. Native reset использовать только
через поддерживаемый конкретным backend способ; немедленное освобождение всей
SDK/TCP памяти и доставку RST не обещать. Неподдержанный backend не должен
молча заменять Drop отправкой HTTP-ответа.

Политика Reply/Drop задаётся явно. Пустой обрыв может вызывать reconnect у клиента;
не объявлять его всегда более дешёвым без измерения суммарной частоты повторов.
Не добавлять скрытого адаптивного QoS или дополнительных потоков ради этого.

## Отключение и конфигурация

Все собственные compile flags/defaults имеют NAWSERVER_, демо — NAWSERVER_DEMO_,
Kconfig генерирует CONFIG_NAWSERVER_. Новые имена ниже — проект, не готовый API.

- NAWSERVER_CLIENT_QUOTAS: исключает quota policy/tables/проверки при 0.
- NAWSERVER_AUTH: независимо исключает authentication providers/code.
- Page identity и NVM API-key provider — отдельные необязательные возможности.
- NAWSERVER_CLIENT_LIMIT и HTTP/WS/SSE quotas задаются системой, не константой 2.
- API key capacity, per-key reserves, TX jobs/bytes, HTTP/header/body/frame
  ёмкости, initial/max allocations и таймауты имеют единые define-defaults.
- Существующие Options позволяют явно переопределить default для экземпляра.
- Отключённые WS/SSE исключаются из расчёта и сборки вместе со своими workers.

AUTH=0/QUOTAS=0: глобальные пределы без этих policy; AUTH=0/QUOTAS=1: quotas
по выбранной anonymous/page/IP идентичности; AUTH=1/QUOTAS=0: auth без quotas;
AUTH=1/QUOTAS=1: совместное использование проверенной identity.
Protected routes при AUTH=0 не становятся public молча: их исключают/настраивают
явно, иначе профиль не запускается. User identity без auth не поддерживается.

Текущий NAWSERVER_TCP_ADMISSION объединяет managed HTTP и optional quotas;
разделить build features так, чтобы исключение quotas сохраняло buffers,
keep-alive и нужные generic header/transport hooks. Сейчас Basic/auth source
входит в CORE; полный compile-out authentication ещё предстоит оформить.

## Формулы ресурсов и примеры

N — число обычных identities, H/W/S — HTTP/WS/SSE permits на identity;
A — сумма защищённых API permits всех ключей; R — ограниченный physical ingress/
reply capacity до классификации; K — дополнительные idle keep-alive вне H.
Если idle уже входит в H, K=0; WS/SSE upgrade не создаёт второй TCP socket.

```
C_sessions = N * (H + W + S) + A + R + K
C_native_sockets >= C_sessions + L + U
```

L — listeners, U — прочие sockets приложения. Это расчёт ёмкости backend,
а не TCP quota algorithm. Middleware использует permits из этого бюджета.
Отдельно считаются PCB/listen/closing/TIME_WAIT, netconn/mailbox, pbuf,
send/receive windows и Wi-Fi buffers. R конечен, поэтому ответ каждому новому
TCP клиенту при произвольной внешней перегрузке не гарантируется.

Пример H=2, A=1, R=1, K=0, L=1, U=0:

| N | Профиль | C_sessions | Native sockets минимум |
| ---: | --- | ---: | ---: |
| 2 | HTTP | 6 | 7 |
| 2 | HTTP + WS | 8 | 9 |
| 2 | HTTP + SSE | 8 | 9 |
| 2 | HTTP + WS + SSE | 10 | 11 |
| 4 | HTTP + WS + SSE | 18 | 19 |

```
M_own_peak = M_fixed + M_client_tables(N) + M_ingress(R)
           + N * (H*M_http + W*M_ws + S*M_sse_metadata)
           + M_api_reserve(A) + M_reply(R) + M_tx_global + M_workers
M_platform_peak = M_native_TCP + M_wifi + M_rtos + M_allocator_overhead
M_required = M_own_peak + M_platform_peak + M_application + M_free_reserve
```

Metadata брать из sizeof целевой ABI. Bodyless GET не требует request body.
Общий TX считается один раз, не отдельной полной копией на каждую identity.
Его capacity покрывает configured per-client и protected reserves. Borrowed
immutable Flash spans не равны такому же объёму RAM. При sendOwned учитывать
одновременно живущие исходный и TX blocks, пока нет передачи самого lease.

Иллюстрация подытога буферов: N=2, H=2; HTTP workspace условно 10 КиБ
(headers+body+copied response), WS arena 16 КиБ (нынешний managed default),
TX 32 КиБ, дополнительный API workspace 10 КиБ, reply buffer 1 КиБ:

| Профиль | HTTP | WS arenas | Общий TX | API + reply | Подытог |
| --- | ---: | ---: | ---: | ---: | ---: |
| HTTP | 40 КиБ | 0 | 32 КиБ | 11 КиБ | 83 КиБ |
| HTTP + WS | 40 КиБ | 32 КиБ | 32 КиБ | 11 КиБ | 115 КиБ |
| HTTP + SSE | 40 КиБ | 0 | 32 КиБ | 11 КиБ | 83 КиБ |
| HTTP + WS + SSE | 40 КиБ | 32 КиБ | 32 КиБ | 11 КиБ | 115 КиБ |

Это арифметический пример, не измеренная вся RAM текущего demo. Добавляются
metadata/ingress, SSE state/worker, stacks, TCP/Wi-Fi, allocator, приложение,
free reserve. В общей TX у SSE нет отдельного постоянного полного payload
на каждую сессию, но остальная стоимость SSE учитывается по общей формуле.
Для Drop-only policy reply buffer может не потребоваться; header/input storage
остаётся. Startup heap reserve отделять от .data/.bss и роста initial->maximum;
не складывать initial и maximum как независимые полные allocations.

## Реализация и приёмка

1. Header-stage существующего middleware executor до body/upgrade; optional
   empty chain, однократное выполнение и перенос результатов в обычную chain.
2. Единые quota policy/identity providers, перенос старого QosHttp policy gate
   в middleware. Managed transport не принимает бизнес-решений.
3. Явные Reply/Drop/Continue; Drop через native owner, без fallback HTTP,
   graceful flush/drain и чтения body. Return permits на всех ошибках.
4. Compile-out auth/quotas/WS/SSE и проверка несовместимых профилей/overflow;
   целевые RAM/Flash/stack измерения для RP/C3/S3, не host sizeof вместо MCU.
5. Тесты: oversized/slow headers/body, Drop до body (нулевые HTTP output bytes),
   partial read/write, cancellation, один owned close, переиспользование handle,
   сохранность соседних WS/SSE, keep-alive, API reserves, WS до 101/SSE до 200.
6. Page identity по вкладкам/reload/lease; API-key generation/revoke/NVM contract,
   Reply vs Drop с учётом reconnect rate. Короткие on-device normal/moderate/
   stress проверки не заменяют отдельный soak.

Отдельно остаётся измеренное ожидание входящих WS/API за длинным HTTP callback.
Ни namespace flags, ни перенос quotas сами по себе эту блокировку не устраняют.

## Повторный разбор стека и композиции middleware — 03.11.2026

Это проверка исходников и уточнение проекта, не реализация и не новая аппаратная
приёмка. Предпочтительна короткая статически составленная цепочка независимых
возможностей. Число middleware не является целью. Не делать один монолит и не
делить каждую отдельную проверку/счётчик в самостоятельный middleware.

### Рекомендуемые границы

| Элемент | Ответственность | Что происходит при отключении |
| --- | --- | --- |
| Identity middleware | Bounded извлечение page/client metadata; peer IP из backend, не из произвольного forwarded header | Нет элемента цепочки и page tables, если они не используются |
| Authentication middleware | Проверить credentials выбранным provider; получить Principal | Нет providers/auth code; protected routes требуют явного решения при конфигурации |
| Access middleware | Проверить политику выбранного маршрута по Principal | Без protected routes не нужен; нельзя выключением превратить protected в public |
| Client quotas middleware | Одной согласованной операцией проверить identity/page/HTTP/WS/SSE и защищённый резерв | Нет quota tables, leases, mutex и quota checks |
| Optional request-rate middleware | Ограничивать частоту отдельно от числа занятых ресурсов | Нет rate counters/timestamps |

Проверку API-key выполняет необязательный provider; она не требует самостоятельного
middleware на каждый тип ключа. Provider возвращает проверенную identity/profile,
а не изменяет TCP. При выключенной authentication такой provider может независимо
служить QoS, как оговорено выше. Разбор routing metadata и HTTP framing — работа
диспетчера/parser, не дополнительные middleware ради количества.

Порядок по умолчанию: framing/route metadata -> identity -> authentication ->
access -> quotas -> body -> прикладная chain/endpoint. Независимый дешёвый предел
частоты по непроверенному источнику допустим до дорогого provider только как явно
выбранная policy; он не должен незаметно отменять доступ защищённого API-key.
Rate limit сообщений существующего WS использует отдельный message hook и может
переиспользовать policy, но не притворяется повторным HTTP middleware.

Один executor с разными стадиями не означает удержание Next между стадиями.
Header chain полностью завершается до асинхронного ожидания body. Дальше передаются
только owned request state, проверенная identity и move-only resource lease.
Header-stage context не разрешает читать body или начинать произвольный streaming.
Нельзя подставлять фиктивный пустой body в старый Request и тем самым создавать
ложную видимость готового запроса. Общий механизм вызова/терминальных результатов
переиспользовать, stage-specific данные и допустимые операции выразить явно.

### Проблемы, найденные в текущих исходниках, и необходимые решения

| Область | Наблюдение / риск | Решение и критерий проверки |
| --- | --- | --- |
| Место запуска | `dispatchHttp` читает body и готовит parameters до выбора handler; обычный TCP path вызывает upgrade до HTTP handler | Early hook до обоих действий; отказ не вызывает readBody/body allocation/101/SSE200 |
| Выбор маршрута | Существующий `canHandle(Request*)` потенциально зависит от уже подготовленного запроса | Ввести header-only route metadata/selection для ранней access policy; тот же route token передавать dispatch. Для произвольного body-dependent handler не обещать раннюю route auth без явного контракта |
| Состояние исполнения | Нынешние Context/Next заимствованы только на время синхронного вызова | Между фазами хранить owned metadata/lease, не Context/Request/Next и не views в переиспользуемый ingress buffer |
| Старый QosHttp | `admit` выбирает jobs по `ticket.client`, reserved buffers и important TX по `client == 0`; stream/idle paths тоже знают policy | Перенести решения и таблицы в middleware; передавать backend нейтральный resource lease/profile. Физический allocator исполняет выбранный бюджет, не классифицирует ключи |
| Составные квоты | Независимые acquire по IP/page/key/protocol дают частичное списание и гонки | Один quota coordinator, одна bounded critical section для проверки/фиксации всех лимитов. Provider/crypto/NVM/I/O вне lock; при allocation failure полный rollback |
| Срок жизни permits | Нынешний route AdmissionMiddleware освобождает ticket при выходе из Next; deferred output/WS/SSE живут дольше | Permit живёт до завершения учитываемой работы; upgrade переносит владение до отправки успеха. Ошибка handshake возвращает всё ровно один раз |
| Keep-alive | Один socket может обслуживать несколько запросов с разными ключами; idle socket продолжает занимать физическое место | На каждый запрос новая классификация/auth; сохранить отдельный bounded учёт idle ownership или закрывать после ответа согласно профилю. Не освобождать все клиентские socket charges, оставляя неучтённый idle |
| Drop | `QueuedTransport::close` сначала flush; cancelPending не освобождает Writing/Filling | Отдельная no-flush отмена owner-ом; pending-close state до возврата активных leases. Не считать слот физически свободным до завершения close |
| Терминальные решения | В обычном TCP execute path результат `handler.handle` сейчас игнорируется; dispatcher добавляет fallback500, managed path может добавить503 | Провести Drop через весь стек до владельца; тест нулевого HTTP output, отсутствия fallback и ровно одного close |
| Неочевидный body prefix | Один native read может уже принести часть body вместе с headers | Обещать отсутствие отдельного body read/allocation после отказа; не обещать, что TCP/ingress вообще не получил байты body |
| Reply при отказе | Непрочитанный body делает дальнейший разбор того же соединения небезопасным; 100 Continue может преждевременно запустить загрузку | После раннего отказа закрывать; не drain произвольный body ради keep-alive. Если поддерживается 100 Continue, отправлять только после допуска |
| API-резерв | Свободный permit не равен свободной RAM/jobs/TX; 4 ключа нельзя обеспечить единственным общим reserved workspace | Валидировать сумму защищённых permits, metadata, рабочих буферов и TX jobs/bytes; ordinary allocations не расходуют защищённую часть |
| Полное заполнение TCP | Middleware не видит запрос до доступного socket и headers; pending/idle/closing тоже занимают место | Общие physical bounds/deadlines и конечный запас ingress/close; клиентских правил в TCP нет. Абсолютной гарантии входа при произвольной перегрузке нет |
| Public routes и auth | Нынешний AuthMiddleware пропускает public route даже при наличии Authorization | Явный режим identity для quotas: public не требует логин; если нужна проверенная user identity, provider вызывается по выбранному правилу. Не наследовать identity предыдущего запроса |
| Дубли заголовков | Разные обработчики key/Authorization/page metadata могут выбирать разные значения | Один bounded разбор и единое правило отклонения неоднозначных значений; page_id не даёт permissions или API reserve |
| Native HTTPD | Общий TCP engine и native IDF HTTPD имеют разных владельцев соединения и upgrade/async пути | Матрица capability для early auth/quota/drop на каждом backend. Неподдержанная обязательная возможность выявляется при настройке/start, а не молча обходится |

Ранний middleware должен быть коротким и bounded: никаких NVM операций, сетевых
вызовов или ожидания очереди в горячем пути. API-key verifier работает с ограниченным
снимком в RAM; обновление/отзыв публикует новую версию согласованно. Политика отзыва
уже открытого WS/SSE определяется отдельно от проверки следующих запросов.
Сохранять текущую сериализацию прикладных callbacks; вынос ранней стадии в другой
поток не должен неявно разрешить параллельные вызовы прежнего auth provider.

### Настоящее отключение возможностей

Состав цепочки определяется сборкой: отключённый элемент не добавляется даже как
no-op. Одновременно исключаются его translation units, таблицы, поля расширений,
locks, workers/timers и зависимости, которые больше никому не нужны. Общие поля
HTTP/parser остаются, если нужны другим включённым функциям. Не обещать исчезновение
всего Principal или mutex из библиотеки, если иной включённый модуль использует их.

Одного `if (!enabled) return next()` недостаточно: это только runtime bypass.
Сейчас auth входит в CORE, а NAWSERVER_TCP_ADMISSION объединяет policy и managed
transport. Разделение source groups и публичных compile definitions обязательно.
Все единицы компиляции, включая приложение, должны видеть одинаковые определения
функций, влияющих на layout/API. Конфликтующие профили диагностировать при сборке
или start; не полагаться только на linker garbage collection.

Проверки: минимальный HTTP; auth-only; quotas-only; auth+quotas; page/API-key
providers отдельно; WS/SSE off и on. Проверять symbols/map, sizeof на целевой ABI,
startup heap и отсутствие лишних workers, а не только успешную компиляцию.
NAWSERVER_AUTH и NAWSERVER_CLIENT_QUOTAS остаются предлагаемыми флагами.

### Быстродействие и границы результата

Измерять header-ready -> решение middleware отдельно от accept -> close. Нужны
число вызовов providers, время policy и ожидания lock, active/idle/closing permits,
отказы по причинам, peak bytes и время освобождения leases. Базовые bounded
счётчики и подробный timing должны отключаться независимо; без логов на каждый
запрос. Сравнить разрешённый запрос, quota Reply и quota Drop под тем же потоком
попыток, включая автоматические reconnect браузера.

Измеренный ранее duplex stress C3 и невосстановившиеся примерно 13 КиБ RAM остаются
отдельными открытыми проблемами. Ограничение числа соединений не ограничивает
частоту команд в одном WS и не разблокирует длинный синхронный HTTP callback.
Нужно отдельно измерить очередь входящих callbacks, продолжение file/source
передачи вне длинного callback и жизненный цикл allocations после close.
Не менять прикладные callbacks на параллельные молча ради этого эксперимента.

Приоритет реализации: ранний stage/route contract -> lifetime/Drop/terminal result
-> перенос единой policy и resource profiles -> compile-out -> page/NVM providers
-> измерения. Сначала проверить общие host contracts и ошибки владения, затем
целевые MCU сборки/допустимые hardware проверки. Эта редакция не меняет прошивку.

## Уточнение владельца: canHandle и Drop

Совместимость со старым canHandle не нужна: API ещё не опубликован. Удалять
механизм выбора через canHandle, мигрировать собственные потребители без legacy
adapter. Статические описания маршрутов выбираются по заголовочной части один раз;
приоритеты exact/prefix, одинаковая обработка пути и method mismatch должны иметь
явный контракт. Побочные действия при поиске (например, HttpCounter) переносятся
в точку учёта/middleware. Это решение для будущей реализации, не текущий код.

Разделять прикладные операции на маршруты разумно при разных permissions, лимитах
тела, профилях ресурсов и жизненных циклах. Один JSON/CBOR endpoint допустим для
связанного протокола, batch или атомарной команды. MCP/JSON-RPC сохраняют свою
протокольную диспетчеризацию. Это выбор приложения; больше URL не означает
автоматически меньшие CPU/latency. Рекомендации по производительности публиковать
после сравнения одинаковых payload/ответов/auth/keep-alive и нагрузки.

Drop принят как явное терминальное состояние всей цепи. Оно запрещает дальнейшую
обработку/новые записи/keep-alive и передаёт отмену владельцу I/O. Нельзя удерживать
stack context между фазами или освобождать активный TX lease преждевременно.
Нулевой HTTP output относится к раннему Drop: уже переданные байты не отзываются.

QosHttp не выбрасывается целиком: сохраняются машина состояний соединений,
управление памятью, таймауты и keep-alive. Из него выделяется клиентская policy;
оставшийся механизм получает имя по ответственности. После обсуждения владелец
поручил реализацию и проверки: native HTTPD удаляем, оставляя общий движок и
платформенные socket adapters. TLS — последующая отдельная возможность.
Существование SDK HTTPS не означает HTTPS в текущем адаптере.

## Обязательное расширение core: потоковый приём и передача

Разделение ответственности после уточнения владельца: сервер предоставляет
штатные способы потокового приёма и передачи; приложение
выбирает подходящий способ и не должно блокировать callback длительным ожиданием.
Полное небольшое тело перед endpoint допустимо. Проблема библиотеки — обязательная
полная буферизация любого тела и задержка соседних запросов внутри библиотечных
операций передачи. Последнее решение владельца исключает общий deferred HTTP API:
не удерживать исходный запрос ради ожидания произвольного прикладного результата.
Долгая операция сразу возвращает статус/job id, результат доставляется через WS/SSE
или последующий HTTP polling. Прикладной пул заданий тоже ограничен и отклоняет
лишние задания; смена протокола сама по себе не ограничивает потребление памяти.
Это заменяет предыдущее предложение общего отложенного HTTP-завершения.

- Route явно выбирает отсутствие body, bounded buffered body или streaming body.
  Для небольших запросов сохраняется простой доступ к целому телу; для больших
  отдельный документированный штатный API с ограниченной памятью. Общий предел
  размера загрузки независим от размера рабочего блока; нет скрытого переключения
  между режимами при достижении порога.
- Streaming body — общий механизм передачи байтов обработчику, без зависимости
  от OTA, FS или SD. Потребители: файл изображения, SD-файл, firmware writer,
  потоковый decoder. Общий контракт задаёт начало, приём порции, pause/resume,
  завершение и отмену; отдельно определяет, когда можно переиспользовать буфер.
  Storage mounts, выбор файла/раздела и публикация результата принадлежат приложению.
- При ожидании сети или свободного TX dispatcher продолжает
  обслуживать другие соединения. Streaming требует паузы/продолжения и bounded
  работы за шаг; один синхронный цикл чтения всего файла проблему не решает.
- Upload/download сохраняют ограниченное состояние передачи и leases. Это не
  общий API удержания Request/Response для позднего прикладного ответа. Число
  передач, память и таймауты ограничены; при заполнении отказ. Отмена/таймаут/Drop/
  завершение освобождают ресурсы один раз; порядок HTTP и keep-alive сохраняются.
- Короткие callbacks остаются сериализованными. Тяжёлую CPU/Flash/FS работу нужно
  дробить либо явно передавать исполнителю; произвольный блокирующий callback
  нельзя автоматически сделать неблокирующим. Не вводить task на каждый запрос.
- Проверить медленный upload и download одновременно с быстрым API и входящими
  WS-командами; измерять latency/gaps и peak RAM, а не только полный объём передачи.

OTA и web log — предлагаемые отдельные optional модули поверх этих возможностей,
не часть parser/transport. OTA использует platform provider (на ESP штатный
esp_ota_*), app-owned разметку и политику активации/перезапуска; для RP нельзя
обещать ESP-подобные rollback/slots без отдельной реализации. Не менять FS/NVS
ради модуля. Web log читает bounded журнал приложения; медленный подписчик не
блокирует производителя. Эти модули ещё не реализованы и HTTPD для их общей
HTTP-обвязки не требуется.

RP2350 поддерживает OTA на уровне bootrom: официальный pico-examples
[ota_update](https://github.com/raspberrypi/pico-examples/blob/master/pico_w/wifi/ota_update/README.md)
показывает A/B application partitions и Try Before You Buy с подтверждением образа
через rom_explicit_buy. Это native firmware, не MicroPython. HTTP body sink может
использовать соответствующий platform writer вместо протокола доставки примера.
Возможность чипа не означает, что OTA настроен в нашей демке: нужны подходящая
таблица разделов, сборка образа, запись и политика подтверждения. Не менять текущие
Flash/FS/NVS ради обсуждения и не переносить вывод о RP2350 автоматически на RP2040.

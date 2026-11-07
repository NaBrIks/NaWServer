# Middleware: ранние проверки и прикладные цепочки

Актуальный контракт, 3 ноября 2026. Один синхронный исполнитель обслуживает две
типизированные стадии общего HTTP/WS/SSE двигателя. Он не создаёт задачи и не
выделяет память для прохода самой цепочки; parser, прикладной код и транспорт имеют
свои отдельно ограниченные ресурсы. ESP-IDF HTTPD и `canHandle` удалены.

## Две стадии

| Стадия | Контекст | Назначение |
|---|---|---|
| После заголовков, до тела/upgrade | `HeaderContext`, `HeaderNext`, `HeaderPipeline` | Auth, identity, квоты, ранний отказ |
| После получения тела | `Context`, `Next`, `Pipeline` | Прикладные проверки/метрики и endpoint |

Ранняя стадия получает `HeaderRequest`: метод, путь/query, длину тела, заголовки и
доступный peer address. Читать тело или form через этот объект нельзя.
`HeaderResponse` позволяет установить заголовки, отправить ограниченный ответ или
завершить через Drop. Он не предоставляет file/stream/external API; максимум тела
отказа задаёт `NAWSERVER_HEADER_REPLY_MAX_BYTES` (512 по умолчанию). Превышение
переводит ответ в Drop до отправки. Прикладная стадия получает обычные `Request`
и `Response`.

Сначала выполняется глобальная ранняя цепочка сервера, внутри её последнего
`next()` — ранняя цепочка выбранного маршрута, затем итоговая проверка доступа.
Глобальные auth/квоты нельзя обойти коротким ответом route middleware: они уже
выполнены. Auth может быть глобальным либо локальным для маршрута; итоговый access
check проходит после обеих цепочек, поэтому отсутствие auth не открывает маршрут.

Порядок зависимых middleware задаёт приложение. Если глобальный identity resolver
использует пользователя, auth должен стоять перед ним в глобальной цепочке:
`auth → identity/quota → route chain`. Auth только в route chain выполнится слишком
поздно для глобального user resolver. Явный AccessMiddleware тоже ставится после
соответствующего auth; access-before-auth даёт отказ, а не обход защиты.
Глобальное middleware само может коротко отказать, не вызывая route chain.

## Статическая композиция

```cpp
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/route.h>

using namespace NaWServer;
constexpr Permissions configure = 1u << 0;
// Открытые тестовые значения; не реальные секреты устройства.
static constexpr FixedBasicAuthenticator users("test", "test-only", configure);
static AuthMiddleware auth(users);
static AccessMiddleware access;
static auto private_chain = makeHeaderChain(auth, access);

static Result settings(Context &context)
{
    // context.principal уже содержит проверенные user_id и permissions.
    return responseResult(context.response.text(200, "configured"));
}
static Route private_route("/settings", HttpMethod::Post, settings, {},
    AccessPolicy::require(configure), {}, private_chain);
static HandlerBase *const routes[]{&private_route};
// backend объявлен приложением раньше сервера.
// HttpServer server(backend, routes);
```

Общая цепочка: `HttpServer(backend, routes, limits, defaults, options, early_chain)`
либо `server.setHeaderPipeline(early_chain)` до запуска. Число обработчиков статично;
конструкторы связывают зависимости, `begin()` запускает backend. Таблицы, маршруты,
цепочки, middleware, provider, credentials и borrowed strings живут до успешного
`end()`. Не создавать их временными выражениями.

Три формы ранней цепочки:

```cpp
HeaderChain<AuthMiddleware, AccessMiddleware> typed{auth, access};
auto inferred = makeHeaderChain(auth, access);
HeaderFixedChain<2> counted{headerMiddlewareRef(auth), headerMiddlewareRef(access)};
auto empty = makeHeaderChain();
```

Прикладные аналоги — `Chain`, `makeChain`, `FixedChain`, `middlewareRef`.
Это фасады одного шаблонного исполнителя, не независимые реализации. Они заимствуют
middleware, поддерживают const-callable объекты, запрещают временные middleware и
превращение временной цепочки в borrowed pipeline. Копировать цепочку нельзя.

## before, next, after и терминальное завершение

```cpp
struct CountRequests
{
    unsigned &total;
    Result operator()(HeaderContext &, HeaderNext &next) const
    {
        ++total;
        const auto result = next();
        // Короткая метрика/cleanup; запроса здесь удерживать нельзя.
        return result;
    }
};
```

Для `[A, B]` порядок: A-before → B-before → endpoint → B-after → A-after.
`next()` можно вызвать один раз и только в текущем стеке. Повторный вызов даёт
`InvalidNext`, endpoint второй раз не запускается; ошибка не теряется при
игнорировании результата внешним middleware. Сохранять Next/Request/Response для
другой задачи запрещено.

Middleware может не вызвать next: отправить ответ и вернуть `Complete`, вернуть
`Unauthorized`/`Forbidden` либо `Drop`. Ошибки преобразуются в 401 с optional
WWW-Authenticate, 403 или 500. После начала ответа второй ответ не отправляется;
ошибка завершает соединение. Заголовки задаются до отправки, не в after-фазе.

`Result::Drop` и `response.drop()` — штатное терминальное состояние. Оно запоминается
при возврате из endpoint или middleware: внешнее `Complete` не отменяет Drop.
I/O owner отменяет передачу и закрывает соединение без fallback и финального flush.
Уже отправленные до Drop байты нельзя отозвать. Для нулевого ответа отказывайте
на ранней стадии до отправки.

## Auth и identity

`AccessPolicy` задаёт `public_access()`, `authenticated()` или `require(mask)`.
AuthMiddleware пропускает публичный маршрут без проверки пароля; на защищённом
маршруте требует успешный provider и явно установленный authenticated flag.
AccessMiddleware проверяет все требуемые permission bits. Итоговая проверка перед
допуском/endpoint закрывает маршрут при отсутствии необходимого auth.

Provider: `bool authenticate(HeaderRequest &, Principal &)` и
`const char *challenge()`; методы могут быть const. Challenge — borrowed строка,
живущая до завершения запроса. Provider не должен хранить пароль в Principal или
писать credentials в лог. NVM/FS инициализирует приложение до запуска сервера.

Проверенная identity сохраняется в состоянии текущего запроса и передаётся
прикладной стадии. При повторной проверке тем же provider пароль не декодируется
заново; другой provider проверяется отдельно. Его отказ очищает прежнюю identity.
Новый HTTP-запрос, в том числе keep-alive, начинает с пустой identity.
Обычный отдельный `Pipeline::run` по умолчанию также сбрасывает identity.

`FixedBasicAuthenticator` хранит borrowed username/password, user_id и permissions.
Пустая конфигурация не открывает защищённый маршрут. Чтение Authorization ограничено
`NAWSERVER_AUTHORIZATION_MAX_BYTES` (512); decoder проверяет длину, padding и padding
bits. Общий TCP parser отклоняет дубли запрошенного заголовка. Basic не шифрует
credentials: шифрование не является частью текущего plain HTTP backend.

## Квоты и отключаемые функции

`AdmissionMiddleware` работает только в ранней стадии; `AdmissionRef` предоставляет
политику. Можно задать имя заголовка ключа и необязательный bounded identity resolver.
Распознанный защищённый ключ выбирает выделенный профиль; обычный клиент определяется
resolver. Неизвестный ключ не подменяет выбранный resolver. Подробности страниц,
пользователей, API-ключей и расчёта бюджета — [в руководстве admission](readme_admission_ru.md).

При допуске middleware передаёт request state один move-only resource lease.
Он остаётся у I/O owner, пока соединение использует ресурсы, включая передачу
раннего отказа после уже выполненного допуска. Переход HTTP → WS/SSE переводит тот
же permit в другой класс, не создаёт вторую квоту. Освобождение выполняется ровно
один раз. Backend не разбирает клиентские ключи и не ведёт другую таблицу identity.

`NAWSERVER_AUTH=0` исключает auth provider/middleware и поля identity/provider/challenge в контекстах, а также cached identity/provider в состоянии запроса. User-based identity resolver при этом возвращает InvalidArgument. Защищённая AccessPolicy при
этом не становится публичной: `HttpServer::begin()` возвращает Unsupported.
`NAWSERVER_CLIENT_QUOTAS=0` исключает AdmissionMiddleware и request lease. Эти
опции независимы; отключение квот не отключает физические лимиты TCP, памяти и TX.

Ни одна цепочка не прерывает произвольный долгий callback. Большое тело выбирает
штатный streaming input; большой файл использует owned source. Длительная
прикладная работа возвращает job id и использует polling/WS/SSE; универсального
отложенного HTTP completion API нет.

## Проверки

`pipeline_test.cpp`: порядок фасадов, пустая цепочка, отказ, повторный next,
permissions, очистка/перенос identity, терминальный Drop и no-allocation проход.
`header_routing_test.cpp`: header-only auth до body, выбор маршрута, глобальный
provider, bounded reply, retained quota lease и Drop без чтения/ответа.
Датированный аппаратный [отчёт старого этапа](readme_middleware_verification_ru.md)
сохранён как история; не выдаётся за проверку текущей прошивки.

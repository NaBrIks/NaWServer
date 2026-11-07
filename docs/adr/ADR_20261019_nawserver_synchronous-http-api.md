# Синхронный Request/Response, общие профили и endpoint-сигнатуры

Дата: 2026-10-19. Статус: принято по прямым указаниям владельца в HTTP/API-чате.

## Решение

Обычный HTTP остаётся синхронным. Dispatcher предоставляет один Request и один
некопируемый Response на текущую операцию. Их оболочки размещаются на стеке;
готовое тело ответа заимствуется до возврата send. Владение native request и
правило одного итогового ответа сохраняются без request-owned heap factories.

Поддержать SendResult(Request&,Response&) и SendResult(Response&), а также
существующий middleware-aware Result(Context&), через один исполнитель и одинаковые
проверки AccessPolicy. Отсутствие Request в сигнатуре не отменяет маршрутизацию,
auth/body validation и существование HTTPD request.

Владелец согласовал constexpr Header[] и ResponseOptions с ContentType,
CachePolicy и HeaderFlags. DefaultHeaders становится явной неизменяемой таблицей
с длительным временем жизни; динамический глобальный singleton не требуется.
Разрешить общие server defaults, профиль Route и bounded per-response overrides.
Метаданные готовятся до сетевого I/O; ошибки и повторная отправка возвращаются явно.

Пустые, текстовые, бинарные ответы и redirect доступны через Response;
форматирование использует TextBuffer/память приложения. Нет обязательного new для
response. Контракты body()/copyBodyTo() и decoder сохраняются.

## Последствия

Новый обычный API не добавляет deferred sendLater и не обещает zero-heap для
Request maps/body, HTTPD/lwIP или всей firmware. Adapter takeover имеет отдельный
проверяемый lifecycle и не разрешает удержание stack Next/Request/Response.

Pipeline/provider/permissions/challenge архитектура сохраняется. Замена оставшегося
Basic token verifier относится к реализации provider, а не к переделке middleware.

## Проверка

См. docs/readme_http_api_ru.md, docs/readme_http_api_verification_ru.md и
examples/minimal: обе сигнатуры, profiles/headers, binary/NUL, builder, guard,
повторная отправка, прежние HTTP/WS body cases и C3 LAN acceptance.

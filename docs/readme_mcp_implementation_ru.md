# Embedded MCP и bounded JSON — реализация 2026-10-26

JSON теперь выбирается при сборке: bounded либо yyjson 0.13.0, с общим
ограниченным writer и одинаковым контрактом значений. Размеры и лимиты задаются
через define/Kconfig; API больше не раскрывает Token offsets. Актуальный
[JSON-контракт и настройки](readme_json_backends_ru.md) заменяют первоначальный
bounded-only API, описанный в историческом разделе ниже.

Текущий интерфейс — отдельная страница `#mcp`. В полных демо доступны
`device_info`, `echo`, `experiment_status` и `experiment_set`; весь `/mcp`
защищён прежней Basic-авторизацией команд. INFO сообщает `firmware.json_backend`
и `firmware.mcp_enabled`. Актуальная интеграция и измерения этого расширения
описаны в конце документа; ранние снимки ниже сохранены как checkpoints.

## Выбор backend по первичным источникам

Сначала проверены LwJSON и Frozen, затем остальные кандидаты. Никакой сторонний
код в библиотеку не скопирован, зависимости не устанавливались. Выбран собственный
узкий token-view backend `NAWSERVER_JSON_BOUNDED`, а не DOM и не поиск подстрок.
Это инженерный выбор для данного ограниченного контракта, не сравнительный
benchmark библиотек и не заявление об измеренном преимуществе Flash/RAM на C3.

| Кандидат | Что проверено и почему не выбран для первого backend |
|---|---|
| [LwJSON v1.10 исходник](https://raw.githubusercontent.com/MaJerle/lwjson/develop/lwjson/src/lwjson/lwjson.c), [типы](https://raw.githubusercontent.com/MaJerle/lwjson/develop/lwjson/src/include/lwjson/lwjson.h) | Caller-owned token storage и borrowed strings близки цели. Числовой разбор возвращает один signed integer или real; отдельного точного uint64 типа нет. Строгие преобразования, единый Unicode/duplicate-key policy и writer потребуют дополнительного слоя. |
| [Frozen исходник](https://raw.githubusercontent.com/cesanta/frozen/master/frozen.c) | Callback parser и writer экономны; `json_parse_key` разрешает identifiers, `json_parse_number` разрешает hex, object loop допускает отсутствующий separator. Прямое принятие parser результатов нарушит строгий JSON. Исправление потребует отдельной проверки грамматики; path может усекаться до JSON_MAX_PATH_LEN. |
| [jsmn исходник](https://raw.githubusercontent.com/zserge/jsmn/master/jsmn.h) | Borrowed token offsets удобны; primitive tokens сами не обеспечивают строгую числовую грамматику/checked numeric conversions, Unicode decoding и writer. |
| [cJSON public types](https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h) | Number представлен double и int, поэтому весь uint64 диапазон не сохраняется. DOM с выделением узлов/строк против цели первого MCU backend. |
| [yyjson public API](https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.h) | Полные signed/unsigned/real типы, строгий reader, writer и allocator support — сильный кандидат для будущего backend. Выбранный сейчас borrowed workspace contract не требует materialized document/value arena. Отдельный адаптер/benchmark пока не реализован. |
| [ArduinoJson7 conversions](https://arduinojson.org/v7/api/jsonvariant/as/), [JsonDocument](https://arduinojson.org/v7/api/jsondocument/) | Полный удобный API, integer configuration и DOM ownership требуют отдельной адаптации и контроля allocation/capacity. Не выбран зависимостью общего standalone C++ компонента. |
| Собственный C++17 | Один bounded token scan, проверенные UTF-8/scalars, decoded duplicate comparison, from_chars/to_chars для locale-independent checked чисел. Цена выбора — собственная ответственность за grammar/Unicode/corpus и дальнейшую независимую проверку; библиотека не объявляется полностью fuzz/soak accepted. |

## JSON контракт

Этот раздел сохраняет описание первого bounded-only checkpoint. В текущем API
используются opaque `Value`, byte arena и сменный reader; см. руководство выше.

Публичный `NaWServer/json.h` не раскрывает SDK, OS или сторонний DOM. Вход immutable
borrowed; Token хранит uint16 begin/end/next и type (измерено sizeof=8 на MSVC x64).
Document получает caller-owned token array. Следующий parse инвалидирует все views;
вход должен жить дольше document/views. Документ целиком не копируется. Raw string
interior явно отличается от `decode`, который пишет decoded UTF-8 в caller buffer,
возвращая длину без NUL terminator. Embedded NUL допустим; строки length-based.

Root primitives допустимы. Числовые литералы целиком соответствуют JSON grammar:
без +prefix, leading zero, hex, незавершённой fraction/exponent. Положительные
integer — uint64, отрицательные — int64; преобразование к signed проверяет диапазон.
Real требует fraction/exponent; from_chars не использует locale и отвергает overflow,
underflow вне double range и nonfinite. Writer double сохраняет real lexical form
(`1.0`, а не `1`), max_digits10; NaN/Infinity отвергаются. Нет silent integer→double
fallback. Поддерживаются все JSON escapes, корректные surrogate pairs и UTF-8;
отвергаются lone surrogates, overlong UTF-8, continuation errors, scalar>10FFFF,
unescaped controls. Дубликаты ключей отвергаются по decoded scalar equality,
включая `a`/`\u0061`. Trailing bytes запрещены, whitespace только JSON SP/HT/CR/LF.

Default profile: input4096bytes, decoded string512bytes, container32items,
container depth8, token array задаёт caller (demo128tokens). Capacity/depth/number/
Unicode/duplicate/syntax errors различаются. Failed parse не публикует root/tokens.
Writer пишет только в ограниченный буфер; fragment() явно возвращает незавершённый буфер. view() пустой до успешного finish(callerTokens,capacity), который проверяет полную грамматику тем же bounded parser; после ошибки view также пустой. Append после finish снимает признак завершения. append принимает
trusted validated JSON fragments; string/integer/real делают безопасную кодировку.
Это синхронный buffer writer, не network sink: partial transport writes относятся
к Response/backend. MCP отправляет buffer ровно один раз и проверяет SendResult
через существующее Response state; callback output повторно валидируется тем же
workspace после окончания чтения request views. Overflow/invalid output заменяется
полным JSON-RPC error500, prefix не отправляется.

## MCP: current и явная совместимость

Проверено live official `/specification/latest`: перенаправляет на **2026-07-28**.
[Versioning](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning),
[Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http),
[discover](https://modelcontextprotocol.io/specification/2026-07-28/server/discover).
Эта версия использует per-request metadata, без обязательного initialize handshake.
Поэтому implemented current profile нельзя описывать только старым initialize flow.

`/mcp` POST — полностью autonomous firmware JSON-RPC endpoint. GET/DELETE/другие
методы405 (`Allow: POST`), GET SSE не открыт. JSON request input4096, demo output2048;
один request object, без JSON-RPC batch. Accept должен явно содержать
application/json и text/event-stream с positive q; точные case-insensitive media
tokens, не substring. Content-Type application/json с необязательными parameters.

Current request params._meta обязательно содержит
`io.modelcontextprotocol/protocolVersion:"2026-07-28"` и
`io.modelcontextprotocol/clientCapabilities:{}`. ClientInfo optional. Headers
MCP-Protocol-Version и Mcp-Method повторяют body; tools/call также Mcp-Name.
Поддержан standard `=?base64?...?=` sentinel в Mcp-Name. Missing/mismatched/malformed
mirror metadata400/-32020, missing required body meta400/-32602; unsupported
version400/-32022 с `data.supported` и `data.requested`. Unknown current method404/
-32601. Every current success has `resultType:"complete"`.

Implemented server/discover, ping, tools/list, tools/call. Discover:

```json
{"resultType":"complete","supportedVersions":["2026-07-28","2025-11-25"],"capabilities":{"tools":{}},"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"NaWServer","version":"0.1.0"}}}
```

[2025-11-25 lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
и [transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
имеют отдельную ветвь: initialize валидирует protocolVersion/capabilities/clientInfo,
возвращает supported2025-11-25 + tools capability; notifications/initialized и
прочие notifications202/no body. Последующие requests используют2025 header.
Без session ID, retained session/lifecycle state, server→client requests,
subscriptions/SSE, tasks, sampling, roots, resources/prompts, OAuth implementation,
HTTP+SSE2024 transport или arbitrary device execution. Статический tools список
не меняется; pagination/cursor не поддерживаются. Клиент обязан соблюдать handshake
старого профиля; stateless сервер не хранит чужие capabilities между запросами.
Это ограниченный tools профиль, не полная реализация всех возможностей MCP.

## Application и storage ownership

Core содержит static borrowed Tool array (name/description/inputSchema/callback/
context/readOnly). Callback завершается синхронно и пишет одну text string;
arguments views не могут уйти в очередь. Новые mutating tools потребуют собственную
bounded typed command copy/queue приложения и его policy; данный MVP их не добавляет.

`examples/common/demo_mcp.hpp` — application-owned glue, не библиотечный product API.
Во всех трёх demo: `device_info({})` возвращает target/SDK/uptime/free heap JSON как
MCP text content; `echo({text})` возвращает decoded text. Echo ограничен256 UTF-8
bytes, отдельный MCU resource limit указан в schema description; maxLength256
сам по себе измеряет Unicode codepoints, и не выдаётся за эквивалент byte limit.
Без GPIO/LCD/Wi-Fi изменений. Существующие CBOR APIs сохраняются самостоятельными
CBOR APIs и не выдаются за MCP.

Origin проверяет application callback, default для present Origin — fail closed.
Demo доверяет только actual STA/AP local IPv4 origin http://IP (port80), без
использования untrusted Host как доверенного источника. Originless native clients
допустимы. Hostnames/HTTPS нужно явно добавить в application trusted policy, не
расширять автоматическим доверием Host. Demo tools публичные read-only LAN tools;
развёртывание чувствительных tools требует application middleware/auth.

Endpoint занимает64B на MSVC x64; на32-bit MCU фактический размер измеряется сборкой.
Workspace128*8+2048=3072B один на endpoint, не на connection и не на task stack.
C3/C6 используют static internal arrays. S3 application выделяет один3072B block
MALLOC_CAP_SPIRAM|8BIT до server.begin; Endpoint/atomic guard остаются internal.
Нет PSRAM — endpoint503 без internal fallback. Block живёт до завершения firmware,
не освобождается пока сервер может вызвать handler. No runtime heap allocation
в нашем parser/writer/protocol request path; libc/libstdc++/transport общим zero-heap
не объявляются. Stack включает fixed headers и ограниченную recursive parser depth;
точные MCU high-water/Flash/RAM выводы требуют build/device measurements.

## Конфигурация и проверки

Отсутствующий `NAWSERVER_MCP` =0, `-DNAWSERVER_MCP` =1. Флаги только0/1.
MCP=1 требует NAWSERVER_JSON_BOUNDED=1; unsupported CJSON selection при MCP=1
даёт понятную ошибку. Backend selection сам MCP не включает. IDF Kconfig
CONFIG_NAWSERVER_MCP defaultn и CONFIG_NAWSERVER_JSON_BOUNDED отображаются в те же
public defines; disabled не компилирует parser/MCP и demo glue buffers/routes.
JSON API также можно собирать отдельно явно выбранным source без MCP.

Enabled build profiles: showcase c3_tcp_mcp/c3_tcp_mcp_idf6, C6 c6_tcp_mcp,
S3 s3_tcp_mcp. Они используют отдельные sdkconfig paths и defaults overlay;
обычные profiles оставлены MCP-off. HTTPD и TCP используют общий handler.

Host corpus — tools/mcp, CMake C++17, MSVC /W4 /WX. На checkpoint **135 checks**
passed: strict grammar, exact64 boundaries, escaped duplicate keys, UTF-8/surrogates/
NUL, depth/token/container/string limits, numeric roundtrip, writer overflow,
legacy/current methods, negotiation/metadata/mirror errors, unknown tools/method,
Origin DNS-rebinding regression, media token/q0, base64 header padding/alphabet,
GET/DELETE405. Corpus расширяется вместе с изменениями; итоговое число сверять
с текущим executable. Это не доказательство full parser fuzz acceptance,
interoperability произвольного внешнего MCP client или on-device C3 результата.
SDK builds и hardware координирует parent; этот подагент платы не открывал/не шил.
Коммитов/публикации, установок SDK/runtime/dependencies и расписаний нет.


### Host toolchain limitation

MSVC19.51 (installed VisualStudio2026) собрал strict corpus. Existing Zig0.14.1
с libc++19 не предоставляет floating-point std::from_chars overload и не может
собрать этот backend. C++17 language mode сам по себе недостаточен: требуется
library implementation floating charconv. Непроверенный locale-dependent fallback
не добавлялся; host должен явно выбрать имеющийся MSVC либо проверенный GCC/libstdc++.
SDK GCC14/15 проверяются отдельными parent builds. Три negative config checks
(missing backend, flag=2, conflicting CJSON) отвергаются с ожидаемой диагностикой;
отдельный executable доказывает backend flags alone/MCP-off не включают MCP.

## Проверенная интеграция — 26.10.2026

Финальные frontend assets встроены в образы, после чего платы идентифицированы
по MAC и прошиты штатным esptool с проверкой записанных данных. C6 COM13,
MAC DEVICE-MAC; S3 UART COM11, MAC DEVICE-MAC.

| Профиль | SDK | BIN, байт | data+bss, байт | Проверка |
|---|---|---:|---:|---|
| showcase c3_tcp_mcp | 5.5.6 | 1134160 | 66700 | сборка |
| showcase c3_tcp_mcp_idf6 | 6.1.0 | 1147376 | 68088 | сборка |
| touch_lcd7 s3_tcp_mcp | 5.5.6 | 1412448 | 74808 | сборка + реальная S3 |
| c6_lcd147 c6_tcp_mcp | 5.5.6 | 1208080 | 75632 | сборка + реальная C6 |
| minimal c3_tcp_mcp | 5.5.6 | 991152 | 51588 | сборка |
| minimal c3_tcp_mcp_idf6 | 6.1.0 | 1005648 | 53456 | сборка |

Полные конфигурации/карты памяти/снимки находятся в
[журнале](readme_build_memory_log_ru.md). Это абсолютные размеры текущих demo
images; разницу с прошлым TCP нельзя целиком приписать MCP: одновременно
встроены новые frontend assets. Отдельного matched-assets benchmark нет.

Реальные адреса: [C6](http://192.0.2.1/#lab),
[S3](http://192.0.2.2/#lab); MCP POST `/mcp`. На обеих пройдены modern
discovery/list/device_info/UTF-8 echo, отклонение лишних аргументов/байтового
превышения/враждебного Origin и GET405; также27 проверок прежних HTTP/WS/SSE.
GUI подтвердил те же MCP-операции, source=device и NaWServer TCP; HTTP/WS/SSE
в итоге online на обеих. Отдельный60-секундный WS-сеанс: C6 иS3 по59/59 echo,
телеметрия59/60 соответственно. Boot ID сохранился; ELF SHA256 из INFO совпал
с локальным финальным ELF. Более длительная нагрузка не запускалась.

Runtime snapshot при трёх WS-клиентах: C6 internal free180068/min152584 B;
S3 internal free29119/min6000 B, PSRAM free5569760 B. Это наблюдавшаяся нагрузка,
не idle benchmark. Малый запас internal S3 остаётся ограничением для ночной
нагрузки. MCP workspace S3 расположен в PSRAM, TCP task stack8192 B.

Локальная нативная C++ TCP test-страница:
[8083](http://127.0.0.1:8083/), MCP discovery/list/echo и18/18 HTTP/WS/SSE
прошли в браузере. Красивая модель: [8798](http://127.0.0.1:8798/#lab),
отдельный bounded C# MCP, source=simulator, не прокси к платам; GUI и live smoke
прошли. Старый8788 оставлен: автоматическая проверка отклонила команду
перезапуска без указанной причины. Порт443 не используется.

Повторно проверены46/46 базовых CTest, MCP CTest2/2 (135 checks),65 C# checks,
5 focused MCP frontend tests; полный frontend corpus17 и три Vite/TS builds
прошли в работе UI-подагента. SDK GCC14/15 поддержали floating charconv.
Linux MCP не проверен; ограничение установленного Zig/libc++ описано выше.

Границы: input4096 B, decoded string512 B, depth8, container32 items,
128 tokens, output2048 B. Большой допустимый request ID/экранированный результат
могут превысить output budget: endpoint отвечает HTTP500/JSON-RPC-32603 сid=null;
это явный resource failure, а не обрезанный JSON. Полный fuzz/soak и
interoperability со всеми внешними клиентами не заявлены. Коммит/пуш не делались.

## Страница MCP и управление экспериментами

Страница `#mcp` вынесена из «Лаборатории» в меню «Веб-сервер». На одном экране
расположены список инструментов с описаниями, выбор инструмента, редактируемые
JSON-аргументы и полученные текстовые данные. Полный JSON-RPC ответ и журнал
последних12 обменов раскрываются отдельно. Сырой ответ сохраняется без
пересериализации чисел. На ширине390px панель задач располагается снизу;
таблица инструментов сохраняет колонки и горизонтальную прокрутку.

Полные демо C6/S3/showcase и C# симулятор предоставляют:

| Инструмент | Назначение |
|---|---|
| `device_info` | Чтение сведений об устройстве или модели |
| `echo` | Проверка обмена текстом UTF-8 |
| `experiment_status` | Текущее состояние существующего CPU-эксперимента |
| `experiment_set` | Запуск и остановка через существующий обработчик приложения |

Пример запуска короткого импульсного эксперимента:

```json
{"action":"start","kind":"pulse","duration_ms":1000,"on_ms":20,"period_ms":1000,"core":0,"priority":1}
```

Остановка: `{"action":"stop"}`. Для `experiment_status` аргументы `{}`.
Диапазоны совпадают с прикладным API: duration1000..30000ms, period20..5000ms,
on1..period, priority1..5; C6/C3 поддерживают core0, S3 core0..1. Старт во время
работающего эксперимента отклоняет прежний runtime. MCP не создаёт своих задач
нагрузки и не меняет семантику REST; неверные команды не применяются.

Весь `/mcp` полных демо, включая discovery/чтение, теперь использует тот же
Basic guard, что и существующие команды. Это намеренное изменение доступа
при добавлении записывающих инструментов. UI берёт текущую пару из раздела
«Доступ», хранит её только в текущем сеансе и отправляет в Authorization.
В JSON-аргументы и журнал MCP пароль не записывается. Minimal остаётся
самостоятельным readonly примером с двумя инструментами и прежним доступом.

INFO → Сборка: JSON-библиотека MCP и признак включения MCP берутся из прошивки.
`nawserver_bounded` отображается как **NaWServer bounded JSON**, `system_text_json`
как **System.Text.Json** (C# модель), `none` означает выключенный MCP backend.
На старой прошивке без этих полей отображается `—`, предположений по профилю нет.
Поле относится к JSON-модулю MCP, а не к CBOR-телеметрии и не к SSE serializer.

Четыре реальные tool definitions занимают1233B и140tokens. Workspace увеличен
с128 до256tokens:2048B tokens+2048B output=4096B. На S3 он остаётся в PSRAM
без internal fallback. Двухинструментный Minimal registry проверен отдельно;
ссылка на общий endpoint не требует несуществующего experiment runtime.

Проверки этого этапа:135 основных и33 demo host checks,18 frontend tests,
три TypeScript/Vite профиля; C#84 прямых и149 полных HTTP-проверок без пропусков.
Live smoke сверяет start/status/stop MCP с существующим GET experiments,
проверяет401 без credentials и с неверными credentials до parsing, отклонение
неверного duration без изменения состояния и реальные INFO metadata.
GUI подтвердил запуск pulse на1секунду, автоматическое завершение, status/stop,
отображение того же состояния на странице «Эксперименты», JSON backend в INFO.
Новая модель доступна на http://127.0.0.1:8800/#mcp; старые процессы не заменялись.

### Финальные платы и ограничение S3

Шесть MCP-профилей (showcase/Minimal IDF5.5.6 и6.1.0, C6/S3 IDF5.5.6) собраны;
отдельно native HTTPD/MCP-off C6/S3 — PASS. Финальные BIN: C6 1325376 B,
S3 1527792 B; статические data+bss 76728 /74880 B. Полные config/map/runtime
снимки и хеши находятся в журнале. Workspace4096B учтён; рост Flash включает
новые assets и вещественную сериализацию состояния эксперимента.

На обеих реальных платах отдельный live тест подтвердил start/status/stop через
MCP и то же состояние через прежний REST API. C6 также прошла все27 общих
HTTP/WS/SSE проверок и GUI. INFO на обеих показывает NaWServer bounded JSON,
MCP включён и NaWServer TCP. Прошивки сверены с локальными ELF через INFO.

**S3: стабильность не принята.** Помимо успешного прогона наблюдались HTTP408
при discovery/list и experiment_status; общий board probe завершился408 на
чтении tasks после7 checks. Финальная GUI проверка чтения состояния также408.
Этот ответ появляется при чтении HTTP-заголовков до входа в MCP. Короткий UART
capture не показал перезагрузки/stack overflow/disconnect. В единственном HTTP
worker есть возможная очередь за пустыми accepted соединениями, но причина
конкретного408 не доказана. Отдельное транспортное расследование остаётся открытым;
UI не скрывает ошибку и не повторяет записывающий вызов автоматически.

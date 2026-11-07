# URL decoder и query/form: интеграция, 19 октября 2026

Компонент поддерживает **Lookup** (по умолчанию) и **std::from_chars**,
выбираемые при сборке. Query/form используют общий length-aware decoder contract.

## Выбор backend

Общий header — `include/NaWServer/url_decode.h`, реализация — `src/http/url_decode.cpp`:

```cpp
// Default, no define required:
// #define NAWSERVER_URL_DECODE_BACKEND 1 // Lookup
// Alternative (supply once in build flags for the whole component/application):
// #define NAWSERVER_URL_DECODE_BACKEND 2 // std::from_chars
```

Именованные значения: `NAWSERVER_URL_DECODE_LOOKUP` и `NAWSERVER_URL_DECODE_FROM_CHARS`.
Иное значение вызывает compile-time error. Не задавать разные defines отдельным
translation units: приложение и компонент должны собираться согласованно.
В объекте присутствует только выбранный hex backend; нет runtime dispatch и
второй таблицы «на всякий случай». C++17 — минимальный стандарт. Decoder не
выделяет heap; Lookup использует256 B flash table. from_chars использует API
стандартной библиотеки для пары hex-символов, не готовый std URL parser.

Для независимого примера `examples/minimal`:

```powershell
$env:PLATFORMIO_CORE_DIR=Join-Path (Get-Location) 'local/pio-core'
& %USERPROFILE%\.platformio\penv\Scripts\pio.exe run -d examples/minimal -e c3 -j 2
& %USERPROFILE%\.platformio\penv\Scripts\pio.exe run -d examples/minimal -e c3_std -j 2
```

`c3` выбирает Lookup, `c3_std` добавляет `-DNAWSERVER_URL_DECODE_BACKEND=2`.
Обе C3-конфигурации закрепляют espressif32@6.10.0, ESP-IDF5.4.0,
GCC14.2.0+20241119 и C++17. `build_unflags` удаляет SDK default `gnu++2b`.
В sample `/health` и `/echo` возвращают `X-Url-Decoder` для проверки выбранной сборки.

## Варианты: алгоритмы, преимущества и ограничения

Все варианты преобразуют `%HH` в **один байт**, а не в Unicode-символ.
Например, `%D0%A3` даёт два UTF-8-байта буквы «У». Язык не распознаётся;
разница производительности определяется длиной, количеством escapes и способом
разбора hex. Новые варианты сохраняют один контракт длины/NUL/ошибок.

| Вариант | Алгоритм | Преимущества | Недостатки и подходящий сценарий | Статус |
|---|---|---|---|---|
| Direct | Проверки диапазонов `0…9`, `a…f`/`A…F`, затем `high*16+low` | Нет hex table; компактный и прозрачный код | На каждый hex-символ несколько проверок/ветвлений; на плотном `%HH` уступил Lookup. Кандидат, когда важнее размер flash | Сохранён в эксперименте |
| Lookup | Индексация256-элементной таблицы: байт →0…15 либо invalid; затем сборка output byte | Мало ветвлений, лучший результат на плотном percent-encoding и tested form с escapes; нет heap | Цена256 B flash table. Без `%` преимущество таблицы не используется; cache pressure и размещение кода влияют на результат | Production default, backend1 |
| from_chars | `std::from_chars(first,last,value,16)` на **ровно двух** символах, затем проверка ec и полного потребления | Стандартный C++17 API, нет собственного hex-классификатора, без allocator/locale-dependent conversion | Это общий integer parser, а не готовый URL decoder; проверки полного range остаются обязательны. Скорость/размер зависят от compiler/libstdc++; в проверенном toolchain есть внутренняя256-byte table и дополнительные функции | Production alternative, backend2 |
| Spans | `memchr` ищет `%`, `memmove` переносит обычный участок; escape разбирается арифметически; form отдельно заменяет `+` в участке | Может выигрывать на длинном path почти без escapes за счёт обработки участков | При плотных escapes участки короткие, повторяются вызовы поиска/копирования; form требует ещё прохода для `+`. Проверенная реализация проиграла Lookup на form/encoded Unicode | Сохранён в эксперименте |

**Почему Lookup выбран для скорости:** два обращения к таблице дают hex-значения и признак ошибки. Direct классифицирует символы через диапазоны; from_chars выполняет общий integer parsing; Spans лучше использует длинные участки без escapes. На плотном percent-encoded UTF-8 Lookup оказался быстрее остальных кандидатов. Это результат отдельного прогретого `-Os` benchmark, а не обещание такого же ускорения HTTP handler.

**Почему оставлен from_chars:** владелец предпочёл иметь вариант на стандартном
библиотечном API. Он проходит те же проверки и отличается только hex-стратегией.
Стандартность не отменяет собственные правила percent-encoding, capacity, NUL,
form/path и ошибок. В конкретной библиотеке он тоже может использовать таблицу;
отсутствие собственного LUT не означает отсутствие LUT в прошивке.

Оба production decoder не выделяют память. Память результирующих полей принадлежит
вызывающему API, и полноценный query/form parser выделяет map/string. Числа
code/stack из экспериментального `-Os` отчёта нельзя автоматически переносить на
production consumer с иными флагами; acceptance ниже выполнялся с sample `-Og`.
Полные исходные [таблицы скорости/кода/стека/allocations](readme_url_decode_benchmark_ru.md)
сохранены вместе со всеми пятью вариантами, включая варианты вне production.

## Низкоуровневый контракт

`urlDecode(source,length,destination,capacity,mode)` принимает диапазоны байтов,
возвращает `UrlDecodeResult {status,consumed,written}`. Не читает и не добавляет
NUL-терминатор. Точная capacity допустима; NUL/0xff/UTF-8 — обычные данные.
`Form` превращает raw `+` в пробел, `Path` сохраняет его; `%2B` всегда даёт `+`.
Декодирование однократное (`%2520` → `%20`). UTF-8 не проверяется/не нормализуется.

`%` требует двух ASCII hex-символов. При InvalidEscape consumed указывает на `%`,
при NoSpace — на следующий непоместившийся output byte. Ошибка escape в этой
позиции приоритетнее NoSpace. При отказе может быть записан только полный префикс;
низкоуровневая функция не транзакционная. Exact in-place разрешён, остальной
overlap отвергается до записи. Null с ненулевым extent — BadArgument.

## Контракт HTTP query/form

`src/http/form_parameters.*` получает `std::string_view`, поэтому raw NUL в form body
не обрывает разбор. Парсер разделяет исходные пары по `&`, ключ/значение — по
первому `=`, **затем** декодирует обе части. `%26`/`%3D` остаются данными.

- Пустые пары между `&&` игнорируются. `flag` и `flag=` — присутствующее поле с
  пустым значением; пустой ключ разрешён (`=value`).
- Ключи точные, case-sensitive, включая NUL и UTF-8. `a` и `A` различны;
  `v%61lue` и `value` обозначают один decoded key.
- При повторных decoded keys побеждает **первое** значение. Ошибочный escape
  даже в последующем дубликате отвергает весь источник, а не игнорируется.
- Form имеет приоритет над query, включая присутствующее пустое form-значение.
  Если поля в form нет, используется query. Ошибка form не вызывает fallback.
- Form разбирается только при media type `application/x-www-form-urlencoded`,
  без учёта регистра, с допустимыми parameters/пробелами. Charset не транскодируется.
  JSON/CBOR/octet-stream/multipart и тело без Content-Type не считаются form.
- Оба набора полностью подготавливаются **после bounded body read, до handler
  matching/pipeline/endpoint**. InvalidEscape в любом ключе/значении query или form,
  даже невостребованном, даёт400 `Invalid query/form encoding`; endpoint не вызывается.
  Не публикуются ни частичный источник, ни уже корректная query при ошибке form.
- `parameterParseResult()` и `parameterErrorSource()` позволяют различить Query/Form
  и offset в исходных байтах. На обычном HTTP dispatch ошибочный запрос до handler
  не доходит; getParam при ошибке не выдаёт частичного значения.
- Query берётся из SDK-терминированного `httpd_req_t::uri`; raw NUL в HTTP
  request-target недопустим. `%00` в query поддержан; raw/decoded NUL в form body
  поддержан. Размер URI по-прежнему ограничивает ESP-IDF (512 в minimal), body4096 B.
  Закреплённый SDK также отвергает raw non-ASCII UTF-8 в HTTP request-target до
  вызова компонента: Unicode query передаётся percent-encoded. Raw UTF-8 в form
  body и низкоуровневом decoder поддержан без этой транспортной границы.

`ParameterMap` владеет `std::string` ключами и объектами `ParameterValue` без
отдельного `new/delete` на каждый wrapper. `query(name)`, `form(name)` и
`parameter(name)` возвращают optional borrowed `std::string_view` до уничтожения request.
`body()` остаётся исходным immutable ByteView, парсер его не меняет и не копирует
весь body в промежуточную текстовую строку. Поля материализуются заранее и выделяют
память: весь query/form API **не** объявляется zero-heap.

Ключи декодируются, malformed escapes отвергаются, ключи case-sensitive,
arbitrary body без form Content-Type не используется как источник параметров.
`path()` и `rawQuery()` дают исходные представления URI; decoded поля доступны
через query/form/parameter.

## Использование без потери NUL

```cpp
const auto value = request.parameter("value"); // May contain embedded NUL.
if (value) (void)response.bytes(200, value->data(), value->size());
```

Не передавать такое значение в strlen/strcmp/`send(...,value.c_str())`.
Minimal `/echo` переведён на response с явной длиной. В showcase query_number и
snapshot argument проверяют отсутствие NUL перед старым C-string потребителем,
чтобы `1%00junk` не превращался в допустимое число/идентификатор по префиксу.

## Проверки

Host CMake/CTest **5/5 PASS**: pipeline, RequestBody, экспериментальный decoder,
production Lookup и production from_chars. Каждый production backend:
**78645 checks,0 failures**. Включены все65536 hex-пар, malformed/truncated,
capacity/null/overlap, raw/decoded NUL в ключах/значениях, transactional failure,
duplicates, raw delimiters против encoded, Content-Type,1000 случайных byte
key/value round-trip и все длинные русские/китайские/японские/emoji fixtures.

Minimal содержит эти же production-тесты и loopback HTTPD acceptance. Test main
stack увеличен до8192 B; HTTPD stack6144 и body/URI bounds не увеличены. До self-test
есть3 секунды для подключения USB-консоли после flash. В Wi-Fi credentials
этот этап не нуждается: HTTP/auth/body/WS проверяются через настоящий TCP loopback
и ESP-IDF HTTPD **на C3**, это не host simulator. LAN/Wi-Fi smoke отмечается отдельно.

### Финальная аппаратная проверка

**Обе сборки прошиты и проверены на той же идентифицированной ESP32-C3 rev0.4**:

| Сборка | Production checks | HTTP/auth/body/WS checks | Firmware B | Linker flash B | Static RAM B | Free heap после тестов B |
|---|---:|---:|---:|---:|---:|---:|
| Lookup / `c3` |78645/0 fail|63/0 fail|454112|453708|20444|277552|
| from_chars / `c3_std` |78645/0 fail|63/0 fail|455232|454832|20444|277572|

Реальные HTTP-проверки: encoded NUL и UTF-8 в query; encoded keys; пустое значение
и flag; first duplicate/case sensitivity; однократное декодирование delimiters;
raw/decoded NUL в form и продолжение после него; form/query priority и fallback;
malformed/truncated values, keys, unused/duplicate fields —400; неверная query не
скрывается form; non-form/untyped body не разбирается; длинные1024-byte UTF-8 raw
и3072-byte encoded forms для русского/китайского/японского/emoji; raw Unicode и
raw NUL в HTTP target —400 от SDK; health после отказов, body4096/4097 boundary,
Basic rejection/success/DELETE и WS text/empty/binary4096.

Начальный прогон имел4 FAIL из-за ошибочного ожидания raw UTF-8 в HTTP target.
SDK вернул parse error17/400 до компонента. Этот прогон не засчитан как PASS;
исправлено именно ожидание транспортной границы, SDK strict parser не ослаблялся.
Полные финальные63 проверки повторены на обоих backend. Нормальные WARN при
отрицательных HTTP tests и закрытии тестового WS ожидаемы; selftest failures,
watchdog/panic/stack diagnostics отсутствуют.

Build context: PlatformIO6.1.18, ESP-IDF5.4.0, GCC14.2.0, **C++17, `-Og`**
(сохранённая debug optimization примера), CPU160 МГц, flash DIO80 МГц. C++17 и
`-Og` подтверждены DWARF producer **объекта production url_decode.cpp**, а не только
флагами CMake. Старые microbenchmark timings/frames относятся к другой `-Os`
сборке: новые измерения скорости этим acceptance-прогоном не заявляются.
В текущей сборке собственный frame urlDecode48 B у обоих, from_chars имеет
дополнительные библиотечные frames. Full stack bound по этим цифрам не выводится.

`nm` подтвердил, что Lookup содержит только собственную256-byte hexTable,
from_chars — только libstdc++ hex table/функции. Оба kernel не выделяют heap; full request parser
материализует map/string fields и не является zero-heap.

Firmware SHA-256:

- Lookup: `ae744013c51679695965a50bb04fc2a743da24d29129515322cad99263e9cef7`.
- from_chars: `eebb529899659bd85dc70b93b4c70cbaa139a44fcf952712caf38eead2f39385`.

Перед каждой записью повторно проверены актуальный COM/chip/MAC. Писалось только
app0, esptool hash verification PASS. Bootloader/partition table/data partitions
не записывались, backup не создавался по указанию владельца. Плата оставлена с
**minimal/Lookup**, тестовый HTTPD работает на C3 loopback; Wi-Fi не запущен,
LAN/IP acquisition/network Go smoke этого firmware не проверялись.

Showcase C3 **build PASS**, только два NUL-guard у C-string consumers и закрепление
того же C++17/toolchain. Firmware531008 B, linker flash530608 B, static RAM33624 B;
он не прошивался и его browser/CBOR/RGB поведение не объявляется проверенным этим
этапом. Go smoke unit tests PASS. Прежние unused logging tag/legacy littlefs subtype
warnings сохранены, посторонняя cleanup не выполнялась.

Сводные [verification metadata/hashes](../tools/url_decode/results/integration/verification.json)
содержат обе прошивки и SHA-256 измеренных production/test sources. Все binaries,
сырые serial/flash logs и device identity остаются ignored. Commit/push не выполнялись;
посторонние dirty SSE tools/docs сохранены. локальный монитор стенда существующая запись обновлена,
портал не останавливался и выдуманный endpoint для C3 не регистрировался.

[Benchmark](readme_url_decode_benchmark_ru.md) и все четыре кандидата сохранены.
Оставшиеся задачи HTTP/WS перечислены в
[плане](plans/PLAN_20261019_nawserver_http-ws-follow-up.md).

## Рабочая Wi-Fi сеть: попытка 19 октября 2026

После ввода владельцем локальных credentials собран и прошит **minimal C3 / Lookup**.
Firmware 883664 B, SHA-256
`02e2097ed6a94437e26b1cb59cd4be734ddfe279dce019d237a267e7d608fcbf`.
Linker flash 883268 B, static RAM 29328 B. Рост относительно предыдущей сборки
связан с включением Wi-Fi кода, который при пустом SSID исключался оптимизатором.
Перед записью повторно проверена C3; запись только app0 `0x10000`, hash verification PASS.

На прошитой плате: Lookup **78645/0**, HTTP/WS loopback **63/0**, panic отсутствует.
Wi-Fi сделал 11 неудачных попыток, каждая с reason **201 — WIFI_REASON_NO_AP_FOUND**
(значение проверено в заголовке используемого ESP-IDF 5.4). Достигнут лимит повторов,
IP не получен. Это не доказывает фильтрацию MAC: точка с заданным SSID не найдена.
Проверка HTTP/WS с компьютера через рабочую сеть **не выполнена из-за отсутствия IP**;
loopback не заменяет эту проверку. После настройки сети нужен перезапуск C3 для новых попыток.
MAC передан владельцу в чате; credentials, device identity и сырые логи остаются ignored.

[Metadata этой попытки](../tools/url_decode/results/integration/network-verification.json)
дополняют предыдущую проверку без Wi-Fi; её hashes и результаты выше сохранены.

Повтор после добавления MAC в доступ (19 октября 2026): идентификатор C3 проверен,
выполнен hard reset без перепрошивки. Lookup 78645/0 и loopback HTTP/WS 63/0 снова
PASS. Все 11 попыток Wi-Fi снова завершились `201 — NO_AP_FOUND`; лимит повторов
исчерпан, IP не получен, panic отсутствует. Поэтому LAN HTTP/WS остаются непроверенными.
Добавление MAC не устранило проблему обнаружения точки. Следующий шаг — проверить
точное имя SSID и доступность точки в диапазоне 2,4 ГГц возле C3; текущий результат
не позволяет объявить причиной пароль или фильтрацию MAC.

### Повтор после переноса USB и допуска MAC

19 октября владелец перенёс USB и сообщил о допуске MAC. Повторно найдена та же
C3 (chip/MAC проверены), выполнен hard reset без перепрошивки. Lookup 78645/0 и
HTTP/WS loopback 63/0 снова PASS. Wi-Fi подключился, DHCP выдал IP; reason ошибок
подключения и panic в стартовом capture нет. Адрес и device identity оставлены в
локальном ignored журнале и сообщены владельцу.

С компьютера по рабочей сети `/health` вернул 200, `X-Url-Decoder: lookup`.
Первый smoke прошёл HTTP/form/Basic/DELETE, затем POST получил клиентский timeout
5 s. Последующая health проверка успешна; полный повтор smoke **PASS**: HTTP/form,
Basic/DELETE, partial POST, body limit, binary/NUL body APIs до 4096 B и overflow,
WS text/binary 0..4096 B, пять reconnect и HTTP после WS. Причина первого таймаута
не установлена; успешный повтор не является длительной проверкой стабильности сети.
Текущая попытка заменяет прежний вывод «IP не получен» для оставленной платы;
исторические неудачные попытки и исходные hashes сохранены в metadata.

import {t} from './appearance';
type Entry = [string,string,string,string];
const sections:Record<string,Entry[]>={
 network:[
  ['lwIP','lwIP','Сетевой стек. Общий лимит сокетов задаётся при сборке прошивки.','Network stack. The total socket limit is set when building firmware.'],
  ['HTTPD','HTTPD','HTTP-сервер занимает три служебных сокета.','The HTTP server uses three internal sockets.'],
  ['TCP-сессии','TCP sessions','Соединения HTTPD: обычные HTTP-запросы, keep-alive, WebSocket и SSE.','HTTPD connections: ordinary HTTP requests, keep-alive, WebSocket and SSE.'],
  ['Резерв отказа','Rejection reserve','Приём ограничивается раньше заполнения пула, чтобы осталось место для ответа 503 с Retry-After. При полном истощении сокетов HTTP-ответ не гарантирован.','Admission stops before the pool fills, leaving room for a 503 reply with Retry-After. No HTTP reply is guaranteed when sockets are exhausted.'],
  ['WS / SSE','WS / SSE','Приложение ограничивает число клиентов каждого потока отдельно от общего числа TCP-сессий.','The application limits each stream separately from the total TCP session limit.'],
  ['Retry-After','Retry-After','Рекомендуемая задержка перед повторным запросом. В диагностике показана в миллисекундах; в HTTP-заголовке — в секундах.','Recommended delay before retrying. Diagnostics use milliseconds; the HTTP header uses seconds.'],
  ['NetworkPolicy','NetworkPolicy','Настройки лимитов перед запуском сервера. Это заданные ограничения, а не оценка свободной памяти.','Limits configured before server startup. These are configured bounds, not estimates of free memory.']
 ],
 transport:[
  ['HTTP','HTTP','Начальный и ручной снимок: info, summary, страницы задач. Переключатель HTTP 1 Hz явно включает прежний опрос summary и всех страниц задач раз в секунду. По умолчанию выключен.','Initial/manual snapshot: info, summary and task pages. HTTP 1 Hz explicitly enables summary and all task pages every second. Disabled by default.'],
  ['WebSocket / CBOR','WebSocket / CBOR','Двусторонний бинарный канал: телеметрия и echo.','Bidirectional binary channel for telemetry and echo.'],
  ['SSE / JSON','SSE / JSON','Отдельный односторонний поток диагностических событий. В прошивке использует собственный worker.','Independent one-way diagnostic event stream with its own firmware worker.'],
  ['Доставка','Delivery','У потоков нет replay. Отправка не подтверждает обработку сообщения браузером.','Streams have no replay. Sending does not acknowledge browser processing.']
 ],
 source:[
  ['HTTP','HTTP','Снимок по запросу и команды. Непрерывный опрос summary/tasks включается только переключателем HTTP 1 Hz.','On-demand snapshots and commands. Continuous summary/tasks polling starts only with the HTTP 1 Hz switch.'],
  ['WS','WS','Живые сводки INFO и echo. Выключение закрывает сокет и запрещает переподключение до включения.','Live INFO summaries and echo. Disabling closes the socket and suppresses reconnect until enabled.'],
  ['SSE','SSE','Передаёт диагностический JSON-снимок раз в секунду. Канал работает независимо от WS.','Carries a diagnostic JSON snapshot once per second, independently of WS.'],
  ['Эмулятор','Emulator','CPU, память и задачи — синтетические данные модели.','CPU, memory and tasks are synthetic model data.']
 ],
 counters:[
  ['HTTP / WS','HTTP / WS','Счётчики запросов, кадров и байтов на стороне сервера.','Server-side counts of requests, frames and bytes.'],
  ['SSE','SSE','Количество диагностических событий, принятых этим браузером.','Diagnostic events received by this browser.'],
  ['Очередь отправки','Send queue','Принятое в очередь сообщение ещё не считается доставленным.','An accepted queue item is not a delivery acknowledgement.'],
  ['Клиенты WS','WS clients','Лимит WebSocket-клиентов отличается от общего лимита TCP-сессий.','The WebSocket client limit differs from the total TCP session limit.']
 ],


 memory:[
  ['КиБ','KiB','Таблица использует одну десятичную цифру. Точное число байт доступно в подсказке.','The table uses one decimal place. Tooltips provide exact byte counts.'],
  ['Минимум','Minimum','Минимальное свободное значение с запуска устройства.','Minimum free value since boot.'],
  ['Регионы памяти','Memory regions','Могут пересекаться: значения нельзя складывать.','Regions may overlap and must not be added.'],
  ['Шкала графика','Chart scale','Фиксирована: от нуля до полного объёма внутренней RAM.','Fixed from zero to the full internal RAM capacity.']
 ],
 http:[
  ['Query / form-urlencoded','Query / form-urlencoded','Передают текстовые значения.','Carry text values.'],
  ['CBOR','CBOR','Бинарный формат команд и телеметрии.','Binary format for commands and telemetry.'],
  ['Buffered response','Buffered response','Завершает один HTTP-ответ. Это не поток SSE.','Completes one HTTP response; it is not an SSE stream.']
 ],
 tasks:[
  ['IDLE0 / IDLE1','IDLE0 / IDLE1','Закреплены сверху при любой сортировке и поиске. Панель задач доступна на всех страницах.','Pinned above all sorting and search results. The task dock is available on every page.'],
  ['CPU','CPU','Нормализован на устройство, включая IDLE. Мини-график: 60 секунд, фиксированная шкала 0–100%.','Normalized per device, including IDLE. Mini chart: 60 seconds, fixed 0–100% scale.'],
  ['Состояние','State','▶ выполняется; ✓ готова; песочные часы — ожидание; пауза — приостановлена; × удалена.','Play: running; check: ready; hourglass: blocked; pause: suspended; x: deleted.'],
  ['P','P','Приоритет задачи.','Task priority.'],
  ['C','C','Ядро процессора.','CPU core.'],
  ['B','B','Минимум свободного стека с запуска задачи, в байтах.','Minimum free stack since task creation, in bytes.'],
  ['Выбор задачи','Task selection','Открывает подробности внизу панели.','Opens task details at the bottom of the dock.']
 ]
};
const networkLayers:Record<string,[string,string,string,string]>={
 'lwIP':['ESP-IDF','ESP-IDF','Сетевой стек','Network stack'],
 'HTTPD':['ESP-IDF','ESP-IDF','HTTP-сервер','HTTP server'],
 'TCP-сессии':['Стандарт','Standard','Транспорт','Transport'],
 'Резерв отказа':['Приложение','Application','Политика приёма','Admission policy'],
 'WS / SSE':['Стандарт','Standard','Прикладные протоколы','Application protocols'],
 'Retry-After':['Стандарт','Standard','Протокол HTTP','HTTP protocol'],
 'NetworkPolicy':['Приложение','Application','Конфигурация приложения','Application configuration'],
 'NaWServer':['Библиотека','Library','Серверный API','Server API']
};
sections.network.push(['NaWServer','NaWServer','Передаёт max_connections из ServerOptions в HTTPD и предоставляет API HTTP, WS и SSE. NetworkPolicy и резерв отказа задаются приложением.','Passes ServerOptions.max_connections to HTTPD and exposes HTTP, WS and SSE APIs. NetworkPolicy and the rejection reserve are defined by the application.']);
export function helpEntries(key:string){return (sections[key]??[]).map(([ru,en,ruText,enText])=>{const meta=key==='network'?networkLayers[ru]:undefined;return {term:t(ru,en),description:t(ruText,enText),origin:meta?t(meta[0],meta[1]):'',layer:meta?t(meta[2],meta[3]):''};});}

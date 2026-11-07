'use strict';
const $ = id => document.getElementById(id);
let socket, reconnectTimer, reconnectDelay = 1000, wantSocket = true, paused = false, windowSeconds = 30;
let history = [], lastSample = 0, offline = true, lastMode = '', toastTimer;
const put = (id, value) => { $(id).textContent = value; };
function toast(message) { put('toast', message); $('toast').hidden = false; clearTimeout(toastTimer); toastTimer = setTimeout(() => $('toast').hidden = true, 4000); }
async function request(path, options = {}) {
  const started = performance.now();
  const response = await fetch(path, { ...options, cache: 'no-store', signal: AbortSignal.timeout(4500) });
  return { status: response.status, ok: response.ok, body: await response.text(), ms: Math.round(performance.now() - started) };
}
function time(seconds) { const s = Math.floor(seconds); return `${String(Math.floor(s / 3600)).padStart(2, '0')}:${String(Math.floor(s / 60) % 60).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`; }
function setOffline(message) { offline = true; $('top-dot').classList.remove('online'); put('connection', 'Нет HTTP'); put('sample-state', message); ['heap', 'uptime', 'rssi', 'ws-clients'].forEach(id => put(id, '—')); put('temperature', '— °C'); put('signal-detail', 'Нет свежих данных'); $('heap-meter').style.width = '0'; }
async function poll() {
  try {
    const result = await request('/api/telemetry');
    if (!result.ok) throw new Error(`HTTP ${result.status}`);
    const d = JSON.parse(result.body);
    if (!Number.isFinite(d.uptime_s) || !Number.isFinite(d.temperature_c)) throw new Error('Неверный формат телеметрии');
    offline = false; lastSample = Date.now();
    $('top-dot').classList.add('online'); put('connection', 'HTTP онлайн');
    put('mode', d.mode === 'local' ? 'Локальное демо' : 'ESP32 · устройство');
    if (lastMode !== d.mode) {
      lastMode = d.mode;
      const note = $('source-note');
      note.querySelector('b').textContent = d.mode === 'local' ? 'Локальное демо · все показания синтетические' : 'ESP32 · системные данные реальные, датчик синтетический';
      note.querySelector('p').textContent = d.mode === 'local' ? 'Go-сервер имитирует устройство. Это не подключение к плате и не проверка ESP-IDF.' : 'Heap, uptime и RSSI получены с контроллера. Температура — демонстрационная модель, не физический датчик.';
    }
    put('heap', (d.free_heap / 1024).toFixed(1));
    put('heap-detail', `Минимум с запуска: ${(d.min_heap / 1024).toFixed(1)} КБ`);
    $('heap-meter').style.width = `${Math.min(100, d.free_heap / (400 * 1024) * 100)}%`;
    put('uptime', time(d.uptime_s)); put('rssi', d.rssi_dbm === null ? '—' : d.rssi_dbm);
    put('signal-detail', d.rssi_dbm === null ? 'Wi-Fi не подключён' : d.rssi_dbm > -65 ? 'Уверенный сигнал' : d.rssi_dbm > -78 ? 'Средний сигнал' : 'Слабый сигнал');
    put('ws-clients', d.ws_clients); put('device', d.device); put('address', location.host);
    put('requests', d.http_requests); put('echoes', d.ws_echoes); put('temperature', `${d.temperature_c.toFixed(1)} °C`);
    history.push({ t: lastSample, value: d.temperature_c }); history = history.filter(s => lastSample - s.t <= 90000);
    if (!paused) draw();
    put('sample-state', paused ? 'График на паузе · данные обновляются' : `Опрос 1 Гц · ${history.length} точек в буфере`);
  } catch (e) { setOffline(`Данные недоступны · ${e.message}`); }
  setTimeout(poll, 1000);
}
function draw() {
  const now = Date.now(); const points = history.filter(s => now - s.t <= windowSeconds * 1000);
  let line = '', area = '', previous = 0, segment = [];
  const closeArea = () => { if (segment.length) { area += `M${segment[0].x} 170 ` + segment.map(p => `L${p.x} ${p.y}`).join(' ') + ` L${segment.at(-1).x} 170Z `; segment = []; } };
  points.forEach(s => { const x = 42 + (1 - (now - s.t) / (windowSeconds * 1000)) * 698, y = 170 - (Math.max(24, Math.min(36, s.value)) - 24) / 12 * 150; const gap = s.t - previous > 3000; if (gap) closeArea(); line += `${gap ? 'M' : 'L'}${x.toFixed(1)} ${y.toFixed(1)} `; segment.push({ x, y }); previous = s.t; }); closeArea();
  $('chart-line').setAttribute('d', line); $('chart-area').setAttribute('d', area);
  const last = points.at(-1); $('chart-point').setAttribute('visibility', last ? 'visible' : 'hidden');
  if (last) { $('chart-point').setAttribute('cx', 42 + (1 - (now - last.t) / (windowSeconds * 1000)) * 698); $('chart-point').setAttribute('cy', 170 - (Math.max(24, Math.min(36, last.value)) - 24) / 12 * 150); }
}
setInterval(() => put('last-seen', lastSample ? `${Math.floor((Date.now() - lastSample) / 1000)} с назад${offline ? ' · нет связи' : ''}` : '—'), 1000);
document.querySelectorAll('[data-window]').forEach(button => button.addEventListener('click', () => { windowSeconds = Number(button.dataset.window); document.querySelectorAll('[data-window]').forEach(b => b.classList.toggle('selected', b === button)); put('chart-left', `−${windowSeconds} с`); $('chart').setAttribute('aria-label', `График синтетической температуры за последние ${windowSeconds} секунд`); if (!paused) draw(); }));
$('pause').addEventListener('click', () => { paused = !paused; put('pause', paused ? '▶ Продолжить' : 'Ⅱ Пауза графика'); if (!paused) draw(); put('sample-state', paused ? 'График на паузе · данные обновляются' : 'График обновляется'); });
$('http-method').addEventListener('change', () => put('http-endpoint', $('http-method').value === 'POST' ? 'POST /api/echo · form-urlencoded' : '/api/echo?value=…'));
$('http-form').addEventListener('submit', async event => {
  event.preventDefault(); const button = event.submitter; button.disabled = true;
  const data = new URLSearchParams({ value: $('http-value').value }), method = $('http-method').value;
  try { const r = await request(`/api/echo${method === 'GET' ? '?' + data : ''}`, method === 'POST' ? { method, body: data } : {}); put('http-status', `${r.status} · ${r.ms} мс`); put('http-result', r.body || '(пустой ответ)'); }
  catch (e) { put('http-status', 'Ошибка'); put('http-result', e.message); } finally { button.disabled = false; }
});
function log(message, kind = '') { const row = document.createElement('div'); row.className = kind; row.textContent = `${new Date().toLocaleTimeString('ru-RU')}  ${message}`; $('ws-log').append(row); while ($('ws-log').children.length > 80) $('ws-log').firstElementChild.remove(); $('ws-log').scrollTop = $('ws-log').scrollHeight; }
function wsState(connected, label) { put('ws-state', label); $('ws-dot').classList.toggle('online', connected); $('ws-send').disabled = !connected; put('ws-toggle', wantSocket ? 'Отключить' : 'Подключить'); }
function connect() {
  if (!wantSocket || (socket && socket.readyState < WebSocket.CLOSING)) return;
  wsState(false, 'Подключение…'); socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`); socket.binaryType = 'arraybuffer';
  socket.onopen = () => { reconnectDelay = 1000; wsState(true, 'Соединение открыто'); log('OPEN /ws'); };
  socket.onmessage = event => { if (typeof event.data === 'string') log(`← TEXT ${event.data}`); else { const bytes = new Uint8Array(event.data); log(`← BINARY ${bytes.length} B · ${Array.from(bytes.slice(0, 48), b => b.toString(16).padStart(2, '0')).join(' ')}${bytes.length > 48 ? ' …' : ''}`); } };
  socket.onerror = () => log('Транспорт недоступен', 'error');
  socket.onclose = event => { wsState(false, wantSocket ? 'Ожидание повтора' : 'Отключён вручную'); log(`CLOSE ${event.code}`); if (wantSocket) { reconnectTimer = setTimeout(connect, reconnectDelay); reconnectDelay = Math.min(reconnectDelay * 2, 15000); } };
}
$('ws-toggle').addEventListener('click', () => { wantSocket = !wantSocket; clearTimeout(reconnectTimer); if (wantSocket) connect(); else { socket?.close(1000, 'user'); wsState(false, 'Отключён вручную'); } });
$('ws-form').addEventListener('submit', event => { event.preventDefault(); if (socket?.readyState !== WebSocket.OPEN) return toast('Сначала подключите WebSocket'); const value = $('ws-value').value, bytes = new TextEncoder().encode(value); if (bytes.length > 4096) return toast('Лимит кадра — 4096 байт'); if ($('ws-type').value === 'binary') { socket.send(bytes); log(`→ BINARY ${bytes.length} B`, 'sent'); } else { socket.send(value); log(`→ TEXT ${value}`, 'sent'); } });
$('clear-log').addEventListener('click', () => $('ws-log').replaceChildren());
async function control(withAuth) {
  const headers = {}; if (withAuth) { try { headers.Authorization = 'Basic ' + btoa($('auth-user').value + ':' + $('auth-password').value); } catch { return put('control-result', 'Для этой тестовой пары используйте ASCII.'); } }
  try { const r = await request('/api/control', { method: 'POST', headers, body: new URLSearchParams({ profile: $('profile').value }) }); put('control-result', `${r.status} · ${r.body}`); if (r.ok) toast('Демо-профиль применён в RAM'); }
  catch (e) { put('control-result', `Ошибка · ${e.message}`); }
}
$('control-form').addEventListener('submit', async e => { e.preventDefault(); e.submitter.disabled = true; try { await control(true); } finally { e.submitter.disabled = false; } });
$('deny-test').addEventListener('click', () => control(false));
$('stream-test').addEventListener('click', async () => { try { const r = await request('/api/stream'); put('stream-result', `${r.status} · ${r.body}`); } catch (e) { put('stream-result', e.message); } });
const observer = new IntersectionObserver(entries => entries.forEach(entry => { if (entry.isIntersecting) document.querySelectorAll('nav a').forEach(a => a.classList.toggle('active', a.hash === '#' + entry.target.id)); }), { rootMargin: '-15% 0px -55% 0px', threshold: 0 });
document.querySelectorAll('main>section').forEach(section => observer.observe(section));
window.addEventListener('offline', () => { setOffline('Браузер потерял сеть'); });
poll(); connect();

"use strict";
function createTests(profile) {
const authorization = { Authorization: "Basic " + btoa("test:test-only") };
const cases = [
  { name: "Состояние backend", method: "GET", path: "/health", status: 200, verify: (_, text) => { const value = JSON.parse(text); return value.http === "ok" || value.ok === true; } },
  { name: "Отдельный CSS", method: "GET", path: "/http-test.css", status: 200, verify: (response, text) => response.headers.get("content-type").includes("text/css") && text.includes(":root") },
  { name: "UTF-8 в query", method: "GET", path: profile.parameters + "?value=" + encodeURIComponent("Привет, HTTP!"), status: 200, verify: (_, text) => text === "Привет, HTTP!" },
  { name: "Бинарное тело с NUL", method: "POST", path: profile.binary, status: 200, headers: profile.binaryAuth ? authorization : undefined, body: new Uint8Array([65, 0, 66, 255]), verify: (_, __, bytes) => String(new Uint8Array(bytes)) === "65,0,66,255" },
  { name: "Form перекрывает query", method: "POST", path: profile.parameters + "?value=query", status: 200, body: "value=form", headers: { "Content-Type": "application/x-www-form-urlencoded" }, verify: (_, text) => text === "form" },
  { name: "Некорректный percent escape", method: "GET", path: profile.parameters + "?value=%GG", status: 400 },
  { name: "Отсутствующий маршрут", method: "GET", path: "/not-found", status: 404 },
  { name: "Неподходящий HTTP-метод", method: "GET", path: profile.methodMismatch, status: 404 },
  { name: "Тело больше 4096 байт", method: "POST", path: "/echo", status: 400, body: new Uint8Array(4097) },
  { name: "Защищённый маршрут", method: "GET", path: "/private", status: 401 },
  { name: "Basic: верные credentials", method: "GET", path: "/private", status: 200, headers: authorization },
  profile.forbidden
    ? { name: "Недостаточно прав", method: "GET", path: profile.forbidden, status: 403, headers: authorization }
    : { name: "Basic: неверный пароль", method: "GET", path: "/private", status: 401, headers: { Authorization: "Basic " + btoa("test:wrong") } },
];
if (profile.websocket) {
  cases.push({ name: "WebSocket: текст", method: "WS", path: profile.websocket, status: 101, websocket: true, body: "NaWServer browser echo" });
  cases.push({ name: "WebSocket: binary / NUL", method: "WS", path: profile.websocket, status: 101, websocket: true, body: new Uint8Array([65, 0, 66, 255]) });
}
if (profile.sse) {
  cases.push({ name: "SSE: auth до открытия", method: "GET", path: profile.ssePrivate, status: 401 });
  cases.push({ name: "SSE: UTF-8 / multiline / HTTP / WS", method: "SSE", path: profile.sse, status: 200, sse: true,
    body: "Привет\nстрока 2\n", id: "browser:1", websocketPath: profile.websocket });
  cases.push({ name: "SSE: пустые data / id reset", method: "SSE", path: profile.sse, status: 200, sse: true, body: "", id: "" });
  cases.push({ name: "SSE: NUL в data", method: "SSE", path: profile.sse, status: 200, sse: true, body: "left\0right", id: "browser:3" });
}
return cases;
}
let tests = [];
const table = document.querySelector("#results");
const summary = document.querySelector("#summary");
const button = document.querySelector("#run");
button.disabled = true;
document.querySelector("#address").textContent = location.host;
function showDetails(test, row) {
  for (const item of table.rows) item.classList.toggle("selected", item === row);
  document.querySelector("#detail-title").textContent = test.name;
  document.querySelector("#detail").textContent = test.detail || "Проверка ещё не выполнена.";
}
async function run() {
  if (!tests.length) return;
  button.disabled = true;
  document.body.dataset.result = "running";
  table.replaceChildren();
  let passed = 0;
  for (const test of tests) {
    test.detail = "";
    const row = table.insertRow();
    row.tabIndex = 0;
    const values = [test.name, test.method, test.path, test.status, "—", "—", "Ожидание"];
    for (const value of values) row.insertCell().textContent = String(value);
    for (const index of [3, 4, 5]) row.cells[index].classList.add("number");
    row.cells[1].className = "method";
    row.cells[2].className = "path";
    row.cells[2].title = test.path;
    row.cells[6].className = "pending";
    row.addEventListener("click", () => showDetails(test, row));
    row.addEventListener("keydown", event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); showDetails(test, row); } });
    test.row = row;
  }
  for (const [index, test] of tests.entries()) {
    const row = test.row;
    summary.textContent = `${index + 1} / ${tests.length}`;
    row.cells[6].textContent = "Запрос…";
    const started = performance.now();
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 7000);
    try {
      if (test.sse) {
        test.detail = await sseEvent(test, controller.signal);
        row.cells[4].textContent = "200";
        row.cells[6].textContent = "PASS";
        row.cells[6].className = "pass";
        passed++;
        continue;
      }
      if (test.websocket) {
        test.detail = await websocketEcho(test, controller.signal);
        row.cells[4].textContent = "101";
        row.cells[6].textContent = "PASS";
        row.cells[6].className = "pass";
        passed++;
        continue;
      }
      const response = await fetch(test.path, { method: test.method, body: test.body, headers: test.headers, credentials: "omit", cache: "no-store", signal: controller.signal });
      const bytes = await response.arrayBuffer();
      const text = new TextDecoder().decode(bytes);
      const ok = response.status === test.status && (!test.verify || test.verify(response, text, bytes));
      if (ok) passed++;
      row.cells[4].textContent = String(response.status);
      row.cells[6].textContent = ok ? "PASS" : "FAIL";
      row.cells[6].className = ok ? "pass" : "fail";
      test.detail = `${test.method} ${test.path}\nHTTP ${response.status} · ${bytes.byteLength} bytes\n\n${[...response.headers].map(([name, value]) => `${name}: ${value}`).join("\n")}\n\n${test.body instanceof Uint8Array && response.status === 200 ? "bytes: " + [...new Uint8Array(bytes)].join(" ") : text}`;
    } catch (error) {
      row.cells[6].textContent = "FAIL";
      row.cells[6].className = "fail";
      test.detail = `${test.method} ${test.path}\n${error}`;
    } finally {
      clearTimeout(timeout);
      row.cells[5].textContent = (performance.now() - started).toFixed(1);
    }
  }
  summary.textContent = `${passed} / ${tests.length} пройдено`;
  summary.className = passed === tests.length ? "pass" : "fail";
  document.body.dataset.result = passed === tests.length ? "pass" : "fail";
  document.body.dataset.passed = String(passed);
  button.disabled = false;
  const firstFailure = tests.find(test => test.row.cells[6].textContent === "FAIL");
  showDetails(firstFailure || tests[0], (firstFailure || tests[0]).row);
}
button.addEventListener("click", run);
function websocketEcho(test, signal) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + test.path);
    socket.binaryType = "arraybuffer";
    let completed = false;
    const finish = (error, detail) => {
      if (completed) return;
      completed = true;
      signal.removeEventListener("abort", abort);
      socket.onclose = null;
      socket.onerror = null;
      socket.close();
      if (error) reject(error); else resolve(detail);
    };
    const abort = () => finish(new Error("WebSocket timeout"));
    signal.addEventListener("abort", abort, { once: true });
    socket.onopen = () => socket.send(test.body);
    socket.onmessage = event => {
      if (event.data === "tick") return;
      const binary = test.body instanceof Uint8Array;
      const actual = binary && event.data instanceof ArrayBuffer ? new Uint8Array(event.data) : event.data;
      const matches = binary ? actual instanceof Uint8Array && String(actual) === String(test.body) : actual === test.body;
      finish(matches ? null : new Error("WebSocket echo mismatch"), `WS ${test.path}\nHTTP upgrade 101 · echo PASS\n\n${binary ? "bytes: " + [...actual].join(" ") : actual}`);
    };
    socket.onerror = () => finish(new Error("WebSocket connection error"));
    socket.onclose = () => finish(new Error("WebSocket closed before echo"));
  });
}
function publishPause(signal) {
  return new Promise((resolve, reject) => {
    if (signal.aborted) { reject(new Error("SSE timeout")); return; }
    const abort = () => { clearTimeout(timer); reject(new Error("SSE timeout")); };
    const timer = setTimeout(() => { signal.removeEventListener("abort", abort); resolve(); }, 15);
    signal.addEventListener("abort", abort, { once: true });
  });
}
function sseEvent(test, signal) {
  return new Promise((resolve, reject) => {
    const source = new EventSource(test.path);
    let completed = false;
    const finish = async (error, detail) => {
      if (completed) return;
      completed = true;
      source.close();
      signal.removeEventListener("abort", abort);
      try { await fetch(test.path + "/close", { method: "POST", cache: "no-store", signal }); } catch (_) {}
      if (error) reject(error); else resolve(detail);
    };
    const abort = () => finish(new Error("SSE timeout"));
    signal.addEventListener("abort", abort, { once: true });
    source.onerror = () => finish(new Error("EventSource disconnected"));
    source.addEventListener("connected", async () => {
      try {
        const health = await fetch("/health", { cache: "no-store", signal });
        if (health.status !== 200) throw new Error("HTTP blocked by SSE");
        if (test.websocketPath) await websocketEcho({ path: test.websocketPath, body: "WS beside SSE" }, signal);
        for (let attempt = 0; attempt < 8; attempt++) {
          if (completed) return;
          const response = await fetch(test.path + "/publish?id=" + encodeURIComponent(test.id), {
            method: "POST", body: test.body, headers: { "Content-Type": "text/plain; charset=utf-8" }, signal
          });
          const cause = await response.text();
          if (response.status === 200) break;
          if (response.status !== 409 || cause !== "busy" || attempt === 7)
            throw new Error(`Publish HTTP ${response.status}: ${cause}`);
          if (completed) return;
          await publishPause(signal);
        }
      } catch (error) { finish(error); }
    }, { once: true });
    source.addEventListener("reading", event => {
      const matches = event.data === test.body && event.lastEventId === test.id;
      finish(matches ? null : new Error("SSE data/id mismatch"),
        `SSE ${test.path}\nEventSource · HTTP 200 · event reading\n\nid: ${JSON.stringify(event.lastEventId)}\ndata: ${JSON.stringify(event.data)}\nHTTP/WS coexistence PASS`);
    });
  });
}
async function initialize() {
  try {
    const response = await fetch("/http-test-config.json", { cache: "no-store", credentials: "omit" });
    if (!response.ok) throw new Error(`Profile HTTP ${response.status}`);
    const profile = await response.json();
    document.querySelector("#backend").textContent = "HTTP / " + profile.backend;
    document.querySelector("#page-title").textContent = profile.sse ? "Проверка HTTP, WS и SSE" : profile.websocket ? "Проверка HTTP и WS" : "Проверка HTTP";
    document.querySelector("#transport").textContent = profile.transport;
    document.querySelector("#scope").textContent = profile.scope;
    tests = createTests(profile);
    await run();
  } catch (error) {
    summary.textContent = "Ошибка загрузки профиля";
    summary.className = "fail";
    document.querySelector("#detail").textContent = String(error);
    document.body.dataset.result = "fail";
    document.body.dataset.passed = "0";
  }
}
initialize();

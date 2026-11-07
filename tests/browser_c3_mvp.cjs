"use strict";
// Own Edge instance only. The origin is /health, so the old automatic UI never runs.
const fs = require("node:fs");
const path = require("node:path");

async function scenario(configuration) {
  const operationMs = 3000;
  const end = performance.now() + configuration.remainingMs;
  const workEnd = end - 9000;
  const summary = { passed: false, iterations: 0, iteration_target: 20, sse_clients: 2,
    counters: { health: 0, websocket_text: 0, websocket_binary: 0, publications: 0,
      connected: 0, readings: [0, 0], ignored_ticks: 0 }, latency_max_ms: {}, cleanup: {} };
  const started = performance.now();
  let phase = "baseline";
  let closing = false;
  let socket;
  let pendingEcho = null;
  let pendingReading = null;
  let baseline = null;
  const streams = [];
  const deferred = () => {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    promise.catch(() => {});
    return { promise, resolve, reject };
  };
  const fatal = deferred();
  const closed = deferred();
  const fail = name => { if (!closing) fatal.reject(new Error(name)); };
  const maximum = (name, value) => { summary.latency_max_ms[name] = Math.max(summary.latency_max_ms[name] || 0, value); };
  const bounded = async (name, action, deadline = workEnd, observeFailure = true) => {
    const duration = Math.min(operationMs, deadline - performance.now());
    if (duration <= 0) throw new Error("total_deadline");
    const controller = new AbortController();
    let timer;
    try {
      return await Promise.race([Promise.resolve().then(() => action(controller.signal)),
        new Promise((_, reject) => { timer = setTimeout(() => {
          controller.abort(); reject(new Error(name + "_timeout"));
        }, duration); }), ...(observeFailure ? [fatal.promise] : [])]);
    } finally { clearTimeout(timer); controller.abort(); }
  };
  const request = async (target, options = {}, deadline = workEnd, observeFailure = true) => {
    return bounded("http", async signal => {
      const response = await fetch(target, { ...options, cache: "no-store", signal });
      const text = await response.text();
      if (text.length > 4096) throw new Error("response_limit");
      return { status: response.status, text };
    }, deadline, observeFailure);
  };
  const readStats = async (deadline = workEnd, observeFailure = true) => {
    const response = await request("/events/stats", {}, deadline, observeFailure);
    if (response.status !== 200) throw new Error("stats_status");
    const result = JSON.parse(response.text);
    for (const name of ["active", "begun", "completed", "queued_bytes"]) {
      if (!Number.isSafeInteger(result[name]) || result[name] < 0) throw new Error("stats_shape");
    }
    return result;
  };
  try {
    baseline = await readStats();
    summary.baseline = baseline;
    if (baseline.active !== 0 || baseline.begun !== baseline.completed || baseline.queued_bytes !== 0)
      throw new Error("preexisting_sse_ownership");

    phase = "websocket_open";
    const opened = deferred();
    socket = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
    socket.binaryType = "arraybuffer";
    socket.onopen = () => opened.resolve();
    socket.onerror = () => fail("websocket_error");
    socket.onclose = () => { closed.resolve(); if (!closing) fail("websocket_closed"); };
    socket.onmessage = event => {
      if (closing) return;
      if (event.data === "tick") { summary.counters.ignored_ticks++; return; }
      if (!pendingEcho) { fail("unexpected_websocket_message"); return; }
      let matches;
      if (pendingEcho.binary) {
        const actual = event.data instanceof ArrayBuffer ? new Uint8Array(event.data) : null;
        matches = actual && actual.length === pendingEcho.data.length &&
          actual.every((value, index) => value === pendingEcho.data[index]);
      } else matches = event.data === pendingEcho.data;
      if (matches) pendingEcho.gate.resolve(); else fail("websocket_echo_mismatch");
    };
    await bounded("websocket_open", () => opened.promise);

    phase = "sse_open";
    const connected = [deferred(), deferred()];
    const seenConnected = [false, false];
    for (let index = 0; index < 2; index++) {
      const stream = new EventSource("/events");
      streams.push(stream);
      stream.onerror = () => fail("sse_disconnected");
      stream.addEventListener("connected", () => {
        if (seenConnected[index]) { fail("sse_reconnected"); return; }
        seenConnected[index] = true;
        summary.counters.connected++;
        connected[index].resolve();
      });
      stream.addEventListener("reading", event => {
        if (closing) return;
        if (!pendingReading || event.lastEventId !== pendingReading.id || event.data !== pendingReading.data ||
            pendingReading.seen[index]) { fail("sse_delivery_mismatch"); return; }
        pendingReading.seen[index] = true;
        summary.counters.readings[index]++;
        if (pendingReading.seen.every(Boolean)) pendingReading.gate.resolve();
      });
    }
    await bounded("sse_connected", () => Promise.all(connected.map(value => value.promise)));

    const echo = async (binary, data) => {
      const began = performance.now();
      const gate = deferred();
      pendingEcho = { binary, data, gate };
      try {
        socket.send(data);
        await bounded("websocket_echo", () => gate.promise);
        summary.counters[binary ? "websocket_binary" : "websocket_text"]++;
        maximum(binary ? "websocket_binary" : "websocket_text", performance.now() - began);
      } finally { pendingEcho = null; }
    };
    const prefix = "mvp:" + Date.now().toString(36) + ":" + Math.random().toString(36).slice(2, 8);
    for (let iteration = 0; iteration < 20; iteration++) {
      const iterationStarted = performance.now();
      phase = "health";
      let began = performance.now();
      const health = await request("/health");
      const body = JSON.parse(health.text);
      if (health.status !== 200 || !(body.ok === true || body.http === "ok")) throw new Error("health_response");
      summary.counters.health++;
      maximum("health", performance.now() - began);
      phase = "websocket_text";
      await echo(false, "mvp-text-" + iteration);
      phase = "websocket_binary";
      await echo(true, new Uint8Array([iteration, 0, 255, (iteration * 7) & 255]));
      phase = "sse_publish_delivery";
      const id = prefix + ":" + iteration;
      const data = "Привет " + iteration + "\nleft\0right\nline 2\n";
      const gate = deferred();
      pendingReading = { id, data, gate, seen: [false, false] };
      began = performance.now();
      try {
        const published = await request("/events/publish?id=" + encodeURIComponent(id), {
          method: "POST", body: data, headers: { "Content-Type": "text/plain; charset=utf-8" } });
        if (published.status !== 200 || published.text !== "queued") throw new Error("publish_response");
        summary.counters.publications++;
        maximum("publish", performance.now() - began);
        // Queue admission is insufficient: both retained streams must deliver the exact event.
        await bounded("sse_delivery", () => gate.promise);
        maximum("sse_delivery", performance.now() - began);
      } finally { pendingReading = null; }
      summary.iterations++;
      maximum("iteration", performance.now() - iterationStarted);
      if (iteration !== 19) await bounded("interval", () => new Promise(resolve => setTimeout(resolve, 100)));
    }
    summary.passed = true;
  } catch (_) {
    summary.failure_phase = phase;
  } finally {
    closing = true;
    const cleanupStarted = performance.now();
    for (const stream of streams) stream.close();
    if (socket && socket.readyState !== WebSocket.CLOSED) {
      try {
        socket.close();
        await bounded("websocket_close", () => closed.promise, end, false);
        summary.cleanup.websocket_closed = true;
      } catch (_) { summary.cleanup.websocket_closed = false; }
    } else summary.cleanup.websocket_closed = true;
    // This fixture's close route is global. Do not use it if preexisting SSE owners were observed.
    if (baseline && baseline.active === 0 && baseline.begun === baseline.completed && streams.length) {
      for (let attempt = 0; attempt < 8 && performance.now() < end; attempt++) {
        summary.cleanup.attempts = attempt + 1;
        try {
          await request("/events/close", { method: "POST" }, end, false);
          const after = await readStats(end, false);
          summary.cleanup.stats = after;
          summary.cleanup.begun_delta = after.begun - baseline.begun;
          summary.cleanup.completed_delta = after.completed - baseline.completed;
          // A close200 acknowledges a request, not actual ownership release.
          summary.cleanup.drained = after.active === 0 && after.queued_bytes === 0 &&
            summary.cleanup.begun_delta === 2 && summary.cleanup.completed_delta === 2;
          if (summary.cleanup.drained) break;
        } catch (_) {}
        try { await bounded("cleanup_pause", () => new Promise(resolve => setTimeout(resolve, 100)), end, false); }
        catch (_) { break; }
      }
    }
    maximum("cleanup", performance.now() - cleanupStarted);
    summary.passed = summary.passed && summary.cleanup.websocket_closed === true && summary.cleanup.drained === true;
    summary.duration_ms = performance.now() - started;
    summary.limits = "One persistent WS and two SSE; no MCU panic/serial claim; not a4client or soak gate.";
  }
  return summary;
}

async function main() {
  const { chromium } = require("playwright");
  const base = new URL(process.argv[2]);
  if (base.protocol !== "http:" || base.username || base.password || base.search || base.hash ||
      !["", "/"].includes(base.pathname) || !process.argv[3]) throw new Error("invalid_arguments");
  const output = path.resolve(process.argv[3]);
  fs.mkdirSync(output, { recursive: true });
  const started = Date.now();
  const totalMs = 45000;
  let browser;
  let summary = { passed: false, failure_phase: "browser_start", iteration_target: 20, sse_clients: 2 };
  try {
    browser = await chromium.launch({ channel: "msedge", headless: true, timeout: 3000 });
    const page = await browser.newPage();
    summary.failure_phase = "origin_health";
    const response = await page.goto(base.origin + "/health", { waitUntil: "load", timeout: 3000 });
    if (!response || response.status() !== 200) throw new Error("origin_health");
    summary = await page.evaluate(scenario, { remainingMs: totalMs - (Date.now() - started) - 1000 });
  } catch (_) {} finally {
    if (browser) {
      let timer;
      try { await Promise.race([browser.close(), new Promise(resolve => {
        timer = setTimeout(resolve, Math.max(0, Math.min(3000, totalMs - (Date.now() - started))));
      })]); } finally { clearTimeout(timer); }
    }
    summary.total_duration_ms = Date.now() - started;
    fs.writeFileSync(path.join(output, "mvp-summary.json"), JSON.stringify(summary, null, 2) + "\n");
    console.log(JSON.stringify(summary, null, 2));
  }
  process.exitCode = summary.passed ? 0 : 1;
}

module.exports = { scenario };
if (require.main === module) main().catch(() => { console.error("MVP_ARGUMENT_OR_SETUP_FAILED"); process.exitCode = 1; });
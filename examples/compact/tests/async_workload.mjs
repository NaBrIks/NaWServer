// SPDX-License-Identifier: MIT
// Explicit hardware fixture client. Never flashes, resets, or creates credentials.
import http from 'node:http';
import {setTimeout as pause} from 'node:timers/promises';

function quantiles(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const rank = q => sorted.length ? sorted[Math.max(0, Math.ceil(sorted.length * q) - 1)] : null;
  return {n: sorted.length, p50: rank(.5), p95: rank(.95), p99: rank(.99), max: sorted.at(-1) ?? null};
}

export async function runAsyncWorkload(base, key, {seconds = 20, onResult = () => {}} = {}) {
  const url = new URL(base);
  if (url.protocol !== 'http:') throw Error('This fixture requires explicit plain HTTP');
  const agent = new http.Agent({keepAlive: true, maxSockets: 3, maxFreeSockets: 2});
  async function request(path, method = 'GET', authorized = false) {
    const start = performance.now();
    return await new Promise((resolve, reject) => {
      const req = http.request(new URL(path, url), {agent, method, headers: authorized ? {'X-Client-Key': key} : {}}, response => {
        const chunks = []; let count = 0;
        response.on('data', chunk => { count += chunk.length; if (count > 128 * 1024) response.destroy(Error('Response limit')); else chunks.push(chunk); });
        response.on('error', reject);
        response.on('end', () => {
          if (response.statusCode !== (method === 'POST' ? 202 : 200)) return reject(Error('HTTP ' + response.statusCode + ' ' + path));
          resolve({bytes: Buffer.concat(chunks), ms: performance.now() - start});
        });
      });
      req.setTimeout(5000, () => req.destroy(Error('Timeout ' + path)));
      req.on('error', reject); req.end();
    });
  }
  const snapshot = async path => JSON.parse((await request(path)).bytes);
  const initial = await snapshot('/ui/status');
  if (!initial.firmware.includes('async-check')) { agent.destroy(); throw Error('Expected explicitly installed async fixture'); }
  if (initial.ws || initial.sse) { agent.destroy(); throw Error('External WS/SSE clients invalidate the quiet baseline'); }
  const cores = (await snapshot('/probe/state')).cores.length;
  const scenarios = [
    {name: 'quiet', mode: 0, web: false},
    {name: 'compute_only', mode: 2, web: false},
    {name: 'priority6_only', mode: 3, web: false},
    {name: 'web_only', mode: 0, web: true},
    {name: 'compute50_web', mode: 1, web: true},
    {name: 'compute80_web', mode: 2, web: true},
    {name: 'priority6_web', mode: 3, web: true},
    ...(cores > 1 ? [{name: 'core1_priority6_only', mode: 4, web: false}, {name: 'core1_priority6_web', mode: 4, web: true}] : [])
  ];
  const results = [];
  try {
    for (const scenario of scenarios) {
      let stop = false, measuring = false;
      const sockets = [], controllers = [], jobs = [];
      const result = {...scenario, seconds, api: {ok: 0, errors: 0}, file: {ok: 0, errors: 0, bytes: 0}, ws: [], sse: [], errors: []};
      const apiTimes = [], fileTimes = [];
      let measurementStartedAt = 0, lastFileCompletedAt = 0;
      try {
        // Reset workload before acquiring clients so one scenario cannot load the next.
        await request('/probe/config?mode=0', 'POST'); await pause(250);
        if (scenario.web) {
          for (let i = 0; i < 2; ++i) {
            const entry = {sent: 0, echoes: 0, bad: 0, times: []}, pending = new Map(); result.ws.push(entry);
            const socket = new WebSocket(new URL('/ws', base.replace('http:', 'ws:'))); socket.binaryType = 'arraybuffer'; sockets.push(socket);
            await new Promise((resolve, reject) => {
              const timer = setTimeout(() => reject(Error('WS open timeout')), 8000);
              socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, {once: true});
              socket.addEventListener('error', () => { clearTimeout(timer); reject(Error('WS open error')); }, {once: true});
            });
            socket.addEventListener('close', () => { if (!stop) ++entry.bad; });
            socket.addEventListener('message', event => {
              if (typeof event.data === 'string') return;
              const bytes = Buffer.from(event.data);
              if (bytes.length !== 256) { ++entry.bad; return; }
              const item = pending.get(bytes.readUInt32BE(0));
              if (!item || !item.bytes.equals(bytes)) { ++entry.bad; return; }
              pending.delete(bytes.readUInt32BE(0)); ++entry.echoes; entry.times.push(performance.now() - item.start);
            });
            jobs.push((async () => { while (!stop) {
              if (!measuring) { await pause(50); continue; }
              const bytes = Buffer.alloc(256, 65 + i); bytes.writeUInt32BE(++entry.sent);
              pending.set(entry.sent, {bytes, start: performance.now()}); socket.send(bytes); await pause(500);
            } })());
          }
          for (let i = 0; i < 2; ++i) {
            const entry = {events: 0, max_gap_ms: 0, errors: 0}; result.sse.push(entry);
            const controller = new AbortController(); controllers.push(controller);
            const response = await fetch(new URL('/events', url), {signal: controller.signal});
            if (response.status !== 200) throw Error('SSE HTTP ' + response.status);
            jobs.push((async () => {
              let text = '', last = 0; const decoder = new TextDecoder(), reader = response.body.getReader();
              try { while (!stop) {
                const item = await reader.read(); if (item.done) { if (!stop) ++entry.errors; break; }
                text += decoder.decode(item.value, {stream: true}); let end;
                while ((end = text.indexOf('\n\n')) >= 0) {
                  const event = text.slice(0, end); text = text.slice(end + 2);
                  if (event.startsWith('data:') && measuring) { const now = performance.now(); if (last) entry.max_gap_ms = Math.max(entry.max_gap_ms, now - last); last = now; ++entry.events; }
                }
              } } catch { if (!stop) ++entry.errors; }
            })());
          }
        }
        await request('/probe/config?mode=' + scenario.mode, 'POST'); await pause(250);
        result.beforeProbe = await snapshot('/probe/state'); result.before = await snapshot('/ui/status');
        if (result.beforeProbe.cores.some(core => core.epoch !== result.beforeProbe.epoch)) throw Error('Application epoch not acknowledged');
        const started = performance.now();
        measurementStartedAt = started;
        measuring = true;
        if (scenario.web) {
          jobs.push((async () => { while (!stop) { try {
            const item = await request('/api/state', 'GET', true); JSON.parse(item.bytes); ++result.api.ok; apiTimes.push(item.ms);
          } catch (e) { ++result.api.errors; result.errors.push(String(e)); } await pause(500); } })());
          jobs.push((async () => { while (!stop) { try {
            const item = await request('/probe/file');
            if (item.bytes.length !== 65536 || item.bytes.some(byte => byte !== 0)) throw Error('File content');
            ++result.file.ok; result.file.bytes += item.bytes.length; fileTimes.push(item.ms); lastFileCompletedAt = performance.now();
          } catch (e) { ++result.file.errors; result.errors.push(String(e)); } await pause(250); } })());
        }
        await pause(seconds * 1000);
        measuring = false; stop = true;
        result.afterProbe = await snapshot('/probe/state'); result.after = await snapshot('/ui/status');
        if (result.afterProbe.epoch !== result.beforeProbe.epoch || result.afterProbe.cores.some(core => core.epoch !== result.afterProbe.epoch)) throw Error('Application epoch changed');
        result.wall_ms = performance.now() - started;
        result.cpu_percent = 100 * (result.after.cpu_busy_us - result.before.cpu_busy_us) / (result.after.cpu_sample_us - result.before.cpu_sample_us);
      } catch (e) { result.error = String(e); }
      finally {
        stop = true; controllers.forEach(x => x.abort());
        await Promise.race([Promise.allSettled(jobs), pause(6000)]);
        const drainDeadline = performance.now() + 2000;
        while (result.ws.some(x => x.echoes < x.sent) && performance.now() < drainDeadline) await pause(20);
        sockets.forEach(x => { try { x.close(); } catch {} });
        result.ws.forEach(x => { x.latency = quantiles(x.times); x.unreceived_after_drain = x.sent - x.echoes; delete x.times; });
        result.api.latency = quantiles(apiTimes); result.file.latency = quantiles(fileTimes);
        result.file.elapsed_ms = Math.max(result.wall_ms ?? 0, lastFileCompletedAt - measurementStartedAt);
        result.file.kib_per_second = result.file.bytes / 1024 / (result.file.elapsed_ms / 1000);
        results.push(result); await onResult(result); await request('/probe/config?mode=0', 'POST'); await pause(2000);
      }
    }
  } finally { agent.destroy(); }
  return {platform: initial.platform, firmware: initial.firmware, seconds, results};
}

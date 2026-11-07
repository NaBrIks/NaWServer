#!/usr/bin/env node
import {decode, encode} from 'cborg';
import {createHash} from 'node:crypto';
import {mkdirSync, writeFileSync} from 'node:fs';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {performance} from 'node:perf_hooks';

const scenarios = new Set(['idle', 'rare_api', 'ws', 'sse', 'both', 'half', 'stress']);
const bounds = Object.freeze({
  cborBytes: 256 * 1024, assetBytes: 8 * 1024 * 1024, sseBytes: 64 * 1024,
  httpRecords: 10000, samples: 4096, errors: 128, echoRecords: 4096,
  pendingEchoes: 128, taskPages: 128, clients: 64, cleanupMs: 3000,
});
const outputDirectory = fileURLToPath(new URL('../../local/transmission-bench/', import.meta.url));
const sleep = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

function usage() {
  return `Usage: node tools/load/transmission-scenarios.mjs --base http://DEVICE [options]
  --scenario idle|rare_api|ws|sse|both|half|stress  Default: idle
  --seconds 1..120                                Default: 30
  --echo-hz 0..20                                 Per WS client; half: 2, stress: 5, others: 0
  --label SAFE_NAME                               Optional report label
  --allow-external                                Record external WS contamination
  --help                                          Show this help without contacting a device
Reports: local/transmission-bench/ (ignored)
`;
}

function parseArguments(arguments_) {
  const values = new Map();
  const allowed = new Set(['--base', '--scenario', '--seconds', '--echo-hz', '--label', '--allow-external']);
  for(let index = 0; index < arguments_.length; ++index) {
    const separator = arguments_[index].indexOf('=');
    const name = separator < 0 ? arguments_[index] : arguments_[index].slice(0, separator);
    if(!allowed.has(name)) throw Error(`Unknown option ${name}`);
    if(values.has(name)) throw Error(`Duplicate option ${name}`);
    if(name === '--allow-external') {
      if(separator >= 0) throw Error('--allow-external does not take a value');
      values.set(name, true); continue;
    }
    const value = separator < 0 ? arguments_[++index] : arguments_[index].slice(separator + 1);
    if(!value || value.startsWith('--')) throw Error(`Missing value for ${name}`);
    values.set(name, value);
  }
  if(!values.has('--base')) throw Error('--base is required');
  let origin;
  try { origin = new URL(values.get('--base')); } catch { throw Error('--base must be an HTTP origin'); }
  if(origin.protocol !== 'http:' || !origin.hostname || origin.pathname !== '/' || origin.search ||
      origin.hash || origin.username || origin.password) {
    throw Error('--base must be a plain HTTP origin without credentials, path, query or fragment');
  }
  const scenario = values.get('--scenario') ?? 'idle';
  if(!scenarios.has(scenario)) throw Error(`Unknown scenario ${scenario}`);
  const seconds = Number(values.get('--seconds') ?? 30);
  if(!Number.isInteger(seconds) || seconds < 1 || seconds > 120) throw Error('--seconds must be an integer from 1 to 120');
  const echoHz = Number(values.get('--echo-hz') ?? ({half: 2, stress: 5}[scenario] ?? 0));
  if(!Number.isFinite(echoHz) || echoHz < 0 || echoHz > 20) throw Error('--echo-hz must be a number from 0 to 20');
  if(echoHz > 0 && ['idle', 'rare_api', 'sse'].includes(scenario)) throw Error('--echo-hz requires a scenario with WS clients');
  const label = values.get('--label') ?? 'run';
  if(!/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(label)) {
    throw Error('--label must contain 1..64 ASCII letters/digits/dots/underscores/hyphens and start with a letter/digit');
  }
  return {base: origin.origin, scenario, seconds, echoHz, label, allowExternal: values.has('--allow-external')};
}

function requireValue(condition, message) {
  if(!condition) throw Error(message);
}

function counter(value, name) {
  requireValue((typeof value === 'bigint' && value >= 0n) ||
    (Number.isSafeInteger(value) && value >= 0), `Invalid counter ${name}`);
  return BigInt(value);
}

function counterDifference(after, before, name) {
  const result = counter(after, name) - counter(before, name);
  requireValue(result >= 0n && result <= BigInt(Number.MAX_SAFE_INTEGER), `Counter reversed/overflowed: ${name}`);
  return Number(result);
}

function percentiles(values) {
  if(!values.length) return {count: 0, p50: null, p95: null, max: null};
  const sorted = [...values].sort((first, second) => first - second);
  const at = fraction => sorted[Math.max(0, Math.ceil(sorted.length * fraction) - 1)];
  return {count: sorted.length, p50: at(0.5), p95: at(0.95), max: sorted.at(-1)};
}

async function readBody(response, maximumBytes) {
  const chunks = [];
  let size = 0;
  const reader = response.body?.getReader();
  if(!reader) return new Uint8Array();
  try {
    for(;;) {
      const {done, value} = await reader.read();
      if(done) break;
      size += value.byteLength;
      requireValue(size <= maximumBytes, `Response exceeds ${maximumBytes} bytes`);
      chunks.push(value);
    }
  } catch(error) {
    await reader.cancel().catch(() => {});
    throw error;
  }
  const result = new Uint8Array(size);
  let offset = 0;
  for(const chunk of chunks) { result.set(chunk, offset); offset += chunk.byteLength; }
  return result;
}

async function run(options) {
  const report = {
    ...options, startedUtc: new Date().toISOString(), pass: false, contaminated: false,
    errors: [], failures: [], warnings: [], http: [], samples: [], droppedRecords: {},
    wsMessages: [], echoes: [], echoSent: [], echoSkipped: [], echoRtt: [], maxGap: [],
    firstSummaryDelayMs: [], lastSummaryAgeMs: [],
    sseEvents: 0, sseComments: 0, sseBytes: 0,
    httpTotals: {attempted: 0, completed: 0, failed: 0, bytes: 0},
  };
  const sockets = [];
  const pendingEchoes = [];
  const lastSummaryAt = [];
  const sseController = new AbortController();
  let closing = false, measuring = false, loadActive = false, echoTimer, echoSequence = 0;
  let sseTask = Promise.resolve(), ownHttpRequests = 0;
  const append = (array, record, limit, category) => {
    if(array.length < limit) array.push(record);
    else report.droppedRecords[category] = (report.droppedRecords[category] ?? 0) + 1;
  };
  const recordError = error => append(report.errors, String(error), bounds.errors, 'errors');
  const url = path => new URL(path, options.base).href;
  const observeExternalClients = count => {
    if(!Number.isInteger(count) || count <= sockets.length) return;
    const external = count - sockets.length;
    if(!report.peakObservedExternalClients && !options.allowExternal) recordError(`External WS observer appeared: ${external}`);
    report.peakObservedExternalClients = Math.max(report.peakObservedExternalClients ?? 0, external);
    report.contaminated = true;
  };

  async function cbor(path) {
    ++ownHttpRequests;
    const response = await fetch(url(path), {signal: AbortSignal.timeout(4000), redirect: 'error'});
    requireValue(response.ok, `HTTP ${response.status} ${path}`);
    return decode(await readBody(response, bounds.cborBytes));
  }

  async function taskSnapshot() {
    for(let attempt = 0; attempt < 4; ++attempt) {
      try {
        let offset = 0, snapshotId, boot, capturedMs;
        const rows = [], visited = new Set();
        do {
          requireValue(!visited.has(offset) && visited.size < bounds.taskPages, 'Invalid/unbounded task pagination');
          visited.add(offset);
          const page = await cbor(`/api/v2/tasks?offset=${offset}&limit=8` +
            (snapshotId ? `&snapshot_id=${encodeURIComponent(snapshotId)}` : ''));
          requireValue(Array.isArray(page.data?.items) && page.data.items.length <= 8, 'Invalid task page');
          requireValue(typeof page.boot_id === 'string' && page.boot_id.length > 0 &&
            typeof page.data.snapshot_id === 'string' && page.data.snapshot_id.length > 0, 'Missing task snapshot identity');
          requireValue(page.data.items.every(row => typeof row.id === 'string' && typeof row.name === 'string'),
            'Invalid task identity');
          if(snapshotId !== undefined) {
            requireValue(page.data.snapshot_id === snapshotId && page.boot_id === boot, 'Task snapshot changed between pages');
          }
          snapshotId = page.data.snapshot_id;
          boot = page.boot_id;
          capturedMs ??= page.captured_ms;
          rows.push(...page.data.items);
          offset = page.data.next_offset;
          requireValue(offset === null || (Number.isSafeInteger(offset) && offset >= 0), 'Invalid next task offset');
        } while(offset !== null);
        return {boot, id: snapshotId, capturedMs, rows};
      } catch(error) {
        if(!String(error).includes('HTTP 409') || attempt === 3) throw error;
      }
    }
  }

  async function snapshot() {
    const summary = await cbor('/api/v2/summary');
    const requestSequenceAtSummary = ownHttpRequests;
    requireValue(summary.data && Number.isSafeInteger(summary.data.ws_clients), 'Invalid summary');
    requireValue(typeof summary.boot_id === 'string' && summary.boot_id.length > 0, 'Missing summary boot identity');
    const tasks = await taskSnapshot();
    requireValue(tasks.boot === summary.boot_id, 'Device reset between summary and task snapshot');
    return {summary, tasks, requestSequenceAtSummary};
  }

  async function httpRequest(path, expectedBodyBytes) {
    const start = performance.now();
    const record = {at: Date.now(), path, status: 0, received: 0, contentLength: null, contentEncoding: null};
    const digest = expectedBodyBytes === undefined ? null : createHash('sha256');
    ++ownHttpRequests;
    ++report.httpTotals.attempted;
    try {
      const response = await fetch(url(path), {signal: AbortSignal.timeout(6000), redirect: 'error'});
      record.status = response.status;
      record.contentLength = response.headers.get('content-length');
      record.contentEncoding = response.headers.get('content-encoding');
      requireValue(response.ok, `HTTP ${response.status}`);
      const reader = response.body?.getReader();
      if(reader) {
        try {
          for(;;) {
            const {done, value} = await reader.read();
            if(done) break;
            record.received += value.byteLength;
            requireValue(record.received <= bounds.assetBytes, 'HTTP body exceeded bounded size');
            digest?.update(value);
          }
        } catch(error) { await reader.cancel().catch(() => {}); throw error; }
      }
      if(expectedBodyBytes !== undefined) {
        requireValue(record.received === expectedBodyBytes,
          `Asset length changed/truncated: ${record.received}, expected ${expectedBodyBytes}`);
        record.sha256 = digest.digest('hex');
        requireValue(record.sha256 === report.assetReference.sha256, 'Asset content changed/corrupted');
      }
      record.ms = performance.now() - start;
      ++report.httpTotals.completed;
      report.httpTotals.bytes += record.received;
      append(report.http, record, bounds.httpRecords, 'http');
    } catch(error) {
      ++report.httpTotals.failed;
      record.ms = performance.now() - start;
      record.error = String(error);
      record.cause = String(error.cause?.code ?? error.cause ?? '');
      append(report.failures, record, bounds.errors, 'failures');
      recordError(`${path}: ${error}`);
    }
  }

  async function openWebSocket(index) {
    report.wsMessages[index] = report.echoes[index] = report.echoSent[index] = report.echoSkipped[index] = report.maxGap[index] = 0;
    report.echoRtt[index] = [];
    pendingEchoes[index] = new Map();
    const socketUrl = new URL('/ws', options.base);
    socketUrl.protocol = 'ws:';
    const socket = new WebSocket(socketUrl);
    socket.binaryType = 'arraybuffer';
    sockets.push(socket);
    socket.onclose = event => {
      if(!closing) recordError(`WS ${index} unexpectedly closed: ${event.code} ${event.reason}`);
    };
    socket.onmessage = event => {
      try {
        requireValue(event.data instanceof ArrayBuffer && event.data.byteLength <= bounds.cborBytes, 'Invalid/oversized WS frame');
        const message = decode(new Uint8Array(event.data));
        if(message.type === 'summary' && measuring) {
          const now = Date.now();
          if(lastSummaryAt[index]) report.maxGap[index] = Math.max(report.maxGap[index], now - lastSummaryAt[index]);
          else report.firstSummaryDelayMs[index] = now - report.started;
          lastSummaryAt[index] = now;
          ++report.wsMessages[index];
          const data = message.data;
          observeExternalClients(data.ws_clients);
          append(report.samples, {client: index, at: now, uptime: data.uptime_ms, cpu: data.cpu_busy_pct,
            clients: data.ws_clients, http: data.counters?.http_requests, free: data.memory?.[0]?.free_bytes,
            tx: data.transmission}, bounds.samples, 'samples');
        } else if(message.type === 'echo.response') {
          requireValue(message.data?.text === `probe-${index}`, `Wrong echo data for WS ${index}`);
          const sentAt = pendingEchoes[index].get(message.request_id);
          requireValue(sentAt !== undefined, `Unexpected/duplicate echo ID for WS ${index}`);
          pendingEchoes[index].delete(message.request_id);
          ++report.echoes[index];
          append(report.echoRtt[index], performance.now() - sentAt, bounds.echoRecords, 'echoRtt');
        }
      } catch(error) { recordError(error); }
    };
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error(`WS ${index} startup timeout`)), 5000);
      socket.onopen = () => { clearTimeout(timer); resolve(); };
      socket.onerror = () => { clearTimeout(timer); reject(Error(`WS ${index} error`)); };
    });
    socket.onerror = () => { if(!closing) recordError(`WS ${index} error`); };
  }

  function startSse() {
    let readyResolve, readyReject;
    const ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
    const timer = setTimeout(() => sseController.abort(Error('SSE startup timeout')), 5000);
    sseTask = (async () => {
      try {
        ++ownHttpRequests;
        const response = await fetch(url('/api/v2/events'), {signal: sseController.signal, redirect: 'error'});
        requireValue(response.ok && response.body, `SSE HTTP ${response.status}`);
        requireValue(response.headers.get('content-type')?.startsWith('text/event-stream'), 'Invalid SSE content type');
        clearTimeout(timer);
        readyResolve();
        const reader = response.body.getReader(), decoder = new TextDecoder();
        let pending = '';
        for(;;) {
          const {done, value} = await reader.read();
          if(done) { if(!closing) recordError('SSE unexpectedly ended'); break; }
          if(measuring) report.sseBytes += value.byteLength;
          pending += decoder.decode(value, {stream: true});
          requireValue(pending.length <= bounds.sseBytes, 'SSE event exceeded bounded size');
          pending = pending.replaceAll('\r\n', '\n');
          let boundary;
          while((boundary = pending.indexOf('\n\n')) >= 0) {
            const block = pending.slice(0, boundary);
            if(measuring) {
              if(block.split('\n').some(line => line.startsWith('data:'))) ++report.sseEvents;
              else ++report.sseComments;
            }
            pending = pending.slice(boundary + 2);
          }
        }
      } catch(error) {
        readyReject(error);
        if(!closing) recordError(error);
      } finally { clearTimeout(timer); }
    })();
    return ready;
  }

  function sendEchoes() {
    sockets.forEach((socket, index) => {
      if(socket.readyState !== WebSocket.OPEN) return;
      if(pendingEchoes[index].size >= bounds.pendingEchoes) {
        if(!report.echoSkipped[index]) recordError(`WS ${index} pending echo limit reached`);
        ++report.echoSkipped[index];
        return;
      }
      const requestId = `p${index}-${++echoSequence}`;
      pendingEchoes[index].set(requestId, performance.now());
      ++report.echoSent[index];
      try {
        socket.send(encode({v: 2, type: 'echo.request', request_id: requestId, data: {text: `probe-${index}`}}));
      } catch(error) { recordError(`WS ${index} echo send failed: ${error}`); }
    });
  }

  try {
    report.info = await cbor('/api/v2/info');
    const maximumClients = report.info.limits?.max_ws_clients;
    requireValue(Number.isInteger(maximumClients) && maximumClients >= 0 && maximumClients <= bounds.clients,
      'info.limits.max_ws_clients missing/outside supported bound');
    const initial = await cbor('/api/v2/summary');
    report.externalClients = initial.data?.ws_clients;
    requireValue(Number.isInteger(report.externalClients) && report.externalClients >= 0 && report.externalClients <= maximumClients,
      'Invalid initial WS client count');
    report.contaminated = report.externalClients > 0;
    requireValue(!report.contaminated || options.allowExternal, `External WS clients present: ${report.externalClients}`);
    const requested = {idle: 0, rare_api: 0, ws: 1, sse: 0, both: 1,
      half: Math.ceil(maximumClients / 2), stress: maximumClients}[options.scenario];
    requireValue(!['ws', 'both', 'half', 'stress'].includes(options.scenario) || maximumClients > 0,
      'Scenario requires nonzero WS capacity');
    report.maximumClients = maximumClients;
    report.controlledClients = Math.min(requested, maximumClients - report.externalClients);
    requireValue(!requested || report.controlledClients > 0, 'No WS capacity available for this scenario');
    for(let index = 0; index < report.controlledClients; ++index) await openWebSocket(index);
    if(['sse', 'both', 'half', 'stress'].includes(options.scenario)) await startSse();
    if(options.scenario === 'stress') {
      ++ownHttpRequests;
      const response = await fetch(url('/'), {signal: AbortSignal.timeout(6000), redirect: 'error'});
      requireValue(response.ok, `Asset index HTTP ${response.status}`);
      const html = new TextDecoder().decode(await readBody(response, bounds.cborBytes));
      const source = html.match(/src=["']([^"']+\.js(?:\?[^"']*)?)["']/)?.[1];
      requireValue(source, 'No JS asset referenced by index');
      const asset = new URL(source, options.base);
      requireValue(asset.origin === options.base, 'Cross-origin asset is not allowed');
      ++ownHttpRequests;
      const reference = await fetch(asset, {signal: AbortSignal.timeout(6000), redirect: 'error'});
      requireValue(reference.ok, `Reference asset HTTP ${reference.status}`);
      const bytes = await readBody(reference, bounds.assetBytes);
      requireValue(bytes.byteLength > 0, 'Empty JS asset');
      report.assetReference = {path: asset.pathname + asset.search, bodyBytes: bytes.byteLength,
        sha256: createHash('sha256').update(bytes).digest('hex'),
        contentLength: reference.headers.get('content-length'), contentEncoding: reference.headers.get('content-encoding')};
    }
    await sleep(2000);
    report.before = await snapshot();
    measuring = loadActive = true;
    report.started = Date.now();
    const started = performance.now(), loops = [];
    if(options.scenario === 'rare_api') loops.push((async () => {
      await sleep(options.seconds * 500);
      if(loadActive) await httpRequest('/health');
    })());
    if(options.scenario === 'half') loops.push((async () => {
      while(loadActive) { await httpRequest('/api/v2/summary'); await sleep(1000); }
    })());
    if(options.scenario === 'stress') {
      for(let lane = 0; lane < 2; ++lane) loops.push((async () => {
        let sequence = 0;
        while(loadActive) {
          const asset = new URL(report.assetReference.path, options.base);
          asset.searchParams.set('stress', `${lane}-${sequence++}`);
          await httpRequest(asset.pathname + asset.search, report.assetReference.bodyBytes);
          await sleep(10);
        }
      })());
    }
    const echoIntervalMs = options.echoHz > 0 ? 1000 / options.echoHz : Infinity;
    if(echoIntervalMs <= options.seconds * 1000) echoTimer = setInterval(sendEchoes, echoIntervalMs);
    await sleep(options.seconds * 1000);
    loadActive = false;
    clearInterval(echoTimer);
    await Promise.all(loops);
    report.loadElapsedMs = performance.now() - started;
    report.after = await snapshot(); // CPU boundary precedes bounded late-echo drain.
    report.actualElapsedMs = performance.now() - started;
    report.finished = Date.now();
    measuring = false;
    report.lastSummaryAgeMs = sockets.map((_, index) => lastSummaryAt[index] ? report.finished - lastSummaryAt[index] : null);
    report.maxGap = report.maxGap.map((gap, index) => Math.max(gap,
      report.firstSummaryDelayMs[index] ?? report.actualElapsedMs,
      report.lastSummaryAgeMs[index] ?? report.actualElapsedMs));
    report.observedExternalClients = Math.max(0, report.after.summary.data.ws_clients - sockets.length);
    observeExternalClients(report.after.summary.data.ws_clients);
    requireValue(report.before.tasks.boot === report.after.tasks.boot &&
      report.before.summary.boot_id === report.after.summary.boot_id, 'Device reset during scenario');
    report.httpCounterDelta = counterDifference(report.after.summary.data.counters.http_requests,
      report.before.summary.data.counters.http_requests, 'HTTP requests');
    report.ownHttpRequestsInCounterWindow = report.after.requestSequenceAtSummary - report.before.requestSequenceAtSummary;
    report.txFailedDelta = counterDifference(report.after.summary.data.transmission.failed_jobs,
      report.before.summary.data.transmission.failed_jobs, 'TX failures');
    if(report.txFailedDelta) recordError(`TX failed jobs increased by ${report.txFailedDelta}`);
    if(['idle', 'rare_api'].includes(options.scenario)) {
      if(report.after.summary.data.ws_clients !== 0) {
        report.contaminated = true;
        recordError('External WS observer contaminated quiet scenario');
      }
      if(report.httpCounterDelta > 8) {
        report.contaminated = true;
        recordError(`Background HTTP observer contaminated quiet scenario: ${report.httpCounterDelta} requests`);
      }
    }
    const previous = new Map(report.before.tasks.rows.map(row => [row.id, row]));
    const deltas = [];
    for(const row of report.after.tasks.rows) {
      const prior = previous.get(row.id);
      if(!prior) continue;
      deltas.push({name: row.name, runtimeTicks: counterDifference(row.runtime_ticks, prior.runtime_ticks, 'task runtime'),
        stack_min_free_bytes: row.stack_min_free_bytes});
    }
    const total = deltas.reduce((sum, row) => sum + row.runtimeTicks, 0);
    const idleTasks = deltas.filter(row => row.name.startsWith('IDLE'));
    const idle = idleTasks.reduce((sum, row) => sum + row.runtimeTicks, 0);
    requireValue(total > 0 && idleTasks.length > 0, 'Task runtime/idle counters unavailable');
    report.cpuPercent = 100 * (total - idle) / total;
    report.cpuBasis = 'Cumulative task runtime deltas, normalized by matched-task runtime sum';
    report.cpuWindowMs = counterDifference(report.after.tasks.capturedMs, report.before.tasks.capturedMs, 'Task capture time');
    report.taskPercent = deltas.map(row => ({...row, cpu: 100 * row.runtimeTicks / total})).sort((first, second) => second.cpu - first.cpu);
    report.unmatchedTasks = report.after.tasks.rows.filter(row => !previous.has(row.id)).map(row => row.name);
    const currentIds = new Set(report.after.tasks.rows.map(row => row.id));
    report.retiredTasks = report.before.tasks.rows.filter(row => !currentIds.has(row.id)).map(row => row.name);
    if(report.unmatchedTasks.length || report.retiredTasks.length) {
      report.warnings.push('Task set changed; normalized CPU excludes unmatched lifetimes');
    }
  } catch(error) { recordError(error); }
  finally {
    loadActive = measuring = false;
    clearInterval(echoTimer);
    // A diagnostic failure must not skip accounting for commands already sent.
    // The CPU boundary above stays before this bounded wait for late replies.
    const drainStarted = performance.now();
    while(pendingEchoes.some(pending => pending.size) && performance.now() - drainStarted < bounds.cleanupMs) await sleep(20);
    report.echoDrainMs = performance.now() - drainStarted;
    report.echoPending = pendingEchoes.map(pending => pending.size);
    report.echoLatency = report.echoRtt.map(percentiles);
    report.echoSent.forEach((sent, index) => {
      if(sent !== report.echoes[index] || report.echoPending[index]) {
        recordError(`WS ${index} missing echoes: sent=${sent}, replies=${report.echoes[index]}, pending=${report.echoPending[index]}`);
      }
    });
    if(report.loadElapsedMs > 0) {
      report.completedRequestsPerSecond = report.httpTotals.completed / (report.loadElapsedMs / 1000);
      report.assetsPerSecond = options.scenario === 'stress' ? report.completedRequestsPerSecond : null;
      report.httpBodyBytesPerSecond = report.httpTotals.bytes / (report.loadElapsedMs / 1000);
    }
    report.httpLatency = percentiles(report.http.map(record => record.ms));
    closing = true;
    sseController.abort();
    sockets.forEach(socket => socket.close());
    await Promise.race([sseTask, sleep(bounds.cleanupMs)]);
    await sleep(300);
  }
  report.pass = report.errors.length === 0;
  report.completedUtc = new Date().toISOString();
  mkdirSync(outputDirectory, {recursive: true});
  const filename = `${report.startedUtc.replaceAll(/[:.]/g, '-')}-${options.label}-${options.scenario}.json`;
  const output = join(outputDirectory, filename);
  writeFileSync(output, JSON.stringify(report, (_, value) => typeof value === 'bigint' ? String(value) : value, 2));
  const summary = {...report, info: undefined, before: undefined, after: undefined,
    http: report.http.length, samples: report.samples.length, taskPercent: undefined, echoRtt: undefined, output};
  await new Promise(resolve => process.stdout.write(`${JSON.stringify(summary)}\n`, resolve));
  return report.pass ? 0 : 1;
}

if(process.argv.slice(2).includes('--help')) {
  process.stdout.write(usage());
} else {
  let options;
  try { options = parseArguments(process.argv.slice(2)); }
  catch(error) { process.stderr.write(`${error.message}\n${usage()}`); process.exit(2); }
  process.exit(await run(options));
}

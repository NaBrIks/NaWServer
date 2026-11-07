// Explicit hardware load test. --ota writes the inactive image slot and reboots.
// Key contents are read from a private file and never printed or put in reports.
import {readFileSync, statSync, writeFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import net from 'node:net';

const [baseInput, keyFile, reportFile, durationText, ...options] = process.argv.slice(2);
if(!durationText) throw Error('Usage: node resource-load.mjs BASE KEY_FILE REPORT SECONDS [--slow] [--ota=FILE] [--baseline=REPORT]');
const url = new URL(baseInput);
if(url.protocol !== 'http:' || url.pathname !== '/' || url.search || url.hash || url.username || url.password) throw Error('Use a plain HTTP origin');
const base = url.origin;
const duration = Number(durationText);
if(!Number.isInteger(duration) || duration < 10 || duration > 86400) throw Error('SECONDS must be 10..86400');
const option = prefix => options.find(value => value.startsWith(prefix))?.slice(prefix.length);
const slow = options.includes('--slow');
const overload = options.includes('--overload');
const hostTest = options.includes('--host-test');
if(hostTest && !['127.0.0.1', 'localhost'].includes(url.hostname)) throw Error('--host-test requires loopback');
const otaPath = option('--ota='), baselinePath = option('--baseline=');
const label = option('--label=') ?? 'unnamed';
if(!/^[a-zA-Z0-9_.-]{1,64}$/.test(label)) throw Error('Invalid profile label');
const interval = (name, fallback, minimum) => {
  const value = Number(option('--' + name + '=') ?? fallback);
  if(!Number.isInteger(value) || value < minimum || value > 60000) throw Error('Invalid interval ' + name);
  return value;
};
const apiMs = interval('api-ms', 1000, 100), pageMs = interval('page-ms', 2000, 100);
const metricsMs = interval('metrics-ms', 10000, 1000), abortMs = interval('abort-ms', 30000, 1000);
for(const value of options) if(value !== '--slow' && value !== '--overload' && value !== '--host-test' && !['--ota=', '--baseline=', '--label=', '--api-ms=', '--page-ms=', '--metrics-ms=', '--abort-ms='].some(prefix => value.startsWith(prefix))) throw Error('Unknown option: ' + value);
const key = readFileSync(keyFile, 'utf8').trim();
if(!/^[0-9a-f]{64}$/.test(key)) throw Error('Key file must contain a 64-character lowercase hex key');
const auth = {'X-Client-Key': key, Connection: 'close'};
const pause = ms => new Promise(resolve => setTimeout(resolve, Math.max(0, ms)));
const report = {started: new Date().toISOString(), label, duration_seconds: duration, slow_second_stream: slow, overload,
  api_interval_ms: apiMs, page_interval_ms: pageMs, metrics_interval_ms: metricsMs, abort_interval_ms: abortMs, metrics_errors: 0,
  streams: [{events: 0, errors: 0, max_gap_ms: 0}, {events: 0, errors: 0, max_gap_ms: 0}],
  api: {ok: 0, errors: 0, rejected: 0, total_ms: 0, max_ms: 0}, page: {ok: 0, errors: 0, rejected: 0, total_ms: 0, max_ms: 0},
  aborted: {started: 0, rejected: 0, errors: 0}, metric_samples: [], dropped_samples: 0, errors: []};
const save = () => writeFileSync(reportFile, JSON.stringify(report, null, 2) + '\n');
const problem = (area, error) => {if(report.errors.length < 100) report.errors.push({area, message: String(error)});};
async function errorDetail(response) {
  if(!response.body) return '';
  const reader = response.body.getReader();
  let bytes = Buffer.alloc(0);
  try {
    // One bounded read: an error response must not hang a test indefinitely.
    const part = await Promise.race([reader.read(), delay(2000).then(() => null)]);
    if(part && !part.done) bytes = Buffer.from(part.value).subarray(0, 128);
  } catch {} finally {reader.cancel().catch(() => {});}
  const detail = bytes.toString('utf8').replaceAll(key, '[redacted]').replace(/[\x00-\x1f\x7f]/g, ' ').trim();
  return detail ? ': ' + detail : '';
}
async function get(path, headers = {}) {
  const response = await fetch(base + path, {headers, signal: AbortSignal.timeout(6000)});
  if(!response.ok) throw Error('HTTP ' + response.status + ' for ' + path + await errorDetail(response));
  return response;
}
async function metrics() {return (await get('/ui/metrics', {Connection: 'close'})).json();}
async function status() {return (await get('/api/status', auth)).json();}
function trackMemory(value) {
  if(typeof value.tx_bytes === 'number') report.tx_observed_max = Math.max(report.tx_observed_max ?? 0, value.tx_bytes);
  if(typeof value.tx_failures === 'number') report.tx_failures_observed_max = Math.max(report.tx_failures_observed_max ?? 0, value.tx_failures);
  if(typeof value.free_heap !== 'number') return;
  report.free_heap_min = Math.min(report.free_heap_min ?? Infinity, value.free_heap);
  if(typeof value.secondary_heap_free === 'number') report.secondary_heap_min = Math.min(report.secondary_heap_min ?? Infinity, value.secondary_heap_free);
  report.largest_block_min = Math.min(report.largest_block_min ?? Infinity, value.largest_free_block);
  if(inOta) {
    report.ota_free_heap_sample_min = Math.min(report.ota_free_heap_sample_min ?? Infinity, value.free_heap);
    if(typeof value.secondary_heap_free === 'number') report.ota_secondary_heap_sample_min = Math.min(report.ota_secondary_heap_sample_min ?? Infinity, value.secondary_heap_free);
  }
}
function recordSample(value) {
  const point = {elapsed_ms: Date.now() - startedAt, cpu_percent: value.cpu_percent,
    free_heap: value.free_heap, min_free_heap: value.min_free_heap, largest_free_block: value.largest_free_block,
    secondary_heap_free: value.secondary_heap_free, stream_clients: value.stream_clients,
    tx_bytes: value.tx_bytes, tx_peak_bytes: value.tx_peak_bytes, tx_failures: value.tx_failures,
    tx_failed_jobs: value.tx_failed_jobs, tx_rejected_jobs: value.tx_rejected_jobs};
  if(report.metric_samples.length < 2048) report.metric_samples.push(point); else report.dropped_samples++;
  trackMemory(value);
}
let startedAt = 0, stop = false, firstPageHash, firstPageBytes = 0, inOta = false, lastSampleAt = 0;
const controllers = [];
const tasks = [];
const apiLatency = [], pageLatency = [];
function sampleLatency(samples, count, value) {
  // Bounded reservoir keeps long-running percentile estimates representative.
  if(samples.length < 8192) samples.push(value);
  else {const index = Math.floor(Math.random() * count); if(index < samples.length) samples[index] = value;}
}
function percentiles(samples) {
  if(!samples.length) return {p95_ms: null, p99_ms: null};
  const sorted = [...samples].sort((a, b) => a - b);
  return {p95_ms: sorted[Math.ceil(sorted.length * 0.95) - 1], p99_ms: sorted[Math.ceil(sorted.length * 0.99) - 1]};
}
let slowSocket;
async function openStream(index) {
  const controller = new AbortController(); controllers.push(controller);
  const timer = setTimeout(() => controller.abort(), 6000);
  let response;
  try {response = await fetch(base + '/ui/events', {signal: controller.signal});}
  finally {clearTimeout(timer);}
  if(!response.ok || !response.headers.get('content-type')?.startsWith('text/event-stream')) throw Error('SSE ' + index + ' HTTP ' + response.status);
  const reader = response.body.getReader(), decoder = new TextDecoder(), stats = report.streams[index];
  tasks.push((async () => {
    let pending = '';
    try {
      while(!stop) {
        const chunk = await reader.read();
        if(chunk.done) {if(!stop) throw Error('unexpected EOF'); break;}
        pending = (pending + decoder.decode(chunk.value, {stream: true})).replace(/\r\n/g, '\n');
        if(pending.length > 16384) throw Error('SSE frame exceeds bounded parser capacity');
        let end;
        while((end = pending.indexOf('\n\n')) >= 0) {
          const frame = pending.slice(0, end); pending = pending.slice(end + 2);
          const data = frame.split('\n').filter(line => line.startsWith('data:')).map(line => line.slice(5).trimStart()).join('\n');
          if(!data) continue;
          const status = JSON.parse(data);
          const now = Date.now();
          if(startedAt) {
            if(index === 0 && now - lastSampleAt >= metricsMs) {recordSample(status); lastSampleAt = now;}
            else trackMemory(status);
            const previous = stats.last_event_ms ?? startedAt;
            stats.max_gap_ms = Math.max(stats.max_gap_ms, now - previous);
            stats.last_event_ms = now;
            stats.events++;
          }
        }
      }
    } catch(error) {if(!stop) {stats.errors++; problem('sse' + index, error);}}
  })());
}
async function openSlowStream() {
  slowSocket = net.connect(Number(url.port) || 80, url.hostname);
  const headers = await new Promise((resolve, reject) => {
    let received = Buffer.alloc(0);
    const timer = setTimeout(() => {slowSocket.destroy(); reject(Error('slow stream header timeout'));}, 6000);
    slowSocket.once('error', reject);
    slowSocket.on('connect', () => slowSocket.write('GET /ui/events HTTP/1.1\r\nHost: ' + url.host + '\r\nAccept: text/event-stream\r\n\r\n'));
    const onData = bytes => {
      received = Buffer.concat([received, bytes]);
      if(received.length > 8192) {clearTimeout(timer); slowSocket.destroy(); reject(Error('slow header too large')); return;}
      if(received.indexOf('\r\n\r\n') < 0) return;
      slowSocket.removeListener('data', onData); slowSocket.pause(); clearTimeout(timer); resolve(received.toString('latin1'));
    };
    slowSocket.on('data', onData);
  });
  if(!/^HTTP\/1\.[01] 200\b/.test(headers)) throw Error('slow stream rejected');
  slowSocket.on('error', error => {if(!stop) {report.streams[1].errors++; problem('slow_stream', error);}});
  slowSocket.on('close', () => {report.streams[1].closed = true;});
  report.streams[1].paused = true;
  report.streams[1].note = 'Receiver paused; this alone does not prove TCP buffers filled.';
}
async function loop(interval, action) {
  while(!stop && Date.now() < deadline) {await action(); await pause(Math.min(interval, Math.max(0, deadline - Date.now())));}
}
async function apiOnce() {
  const at = Date.now();
  try {await status(); const ms = Date.now() - at; report.api.ok++; sampleLatency(apiLatency, report.api.ok, ms); report.api.total_ms += ms; report.api.max_ms = Math.max(ms, report.api.max_ms);}
  catch(error) {if(overload && /^Error: HTTP (429|503)\b/.test(String(error))) report.api.rejected++; else {report.api.errors++; problem('api', error);}}
}
async function pageOnce() {
  const at = Date.now();
  try {
    const response = await get('/', {Connection: 'close'});
    const digest = createHash('sha256'); let size = 0;
    for await(const chunk of response.body) {
      size += chunk.length;
      if(size > 1048576) throw Error('page exceeds 1 MiB test bound');
      digest.update(chunk);
    }
    const hash = digest.digest('hex');
    if(firstPageHash && (firstPageHash !== hash || firstPageBytes !== size)) throw Error('page changed or was truncated');
    firstPageHash = hash; firstPageBytes = size;
    const ms = Date.now() - at; report.page.ok++; sampleLatency(pageLatency, report.page.ok, ms); report.page.total_ms += ms; report.page.max_ms = Math.max(ms, report.page.max_ms);
  } catch(error) {if(overload && /^Error: HTTP (429|503)\b/.test(String(error))) report.page.rejected++; else {report.page.errors++; problem('page', error);}}
}
async function abortPartial() {
  report.aborted.started++;
  await new Promise(resolve => {
    const socket = net.connect(Number(url.port) || 80, url.hostname);
    const timer = setTimeout(() => {socket.destroy(); resolve();}, 300);
    socket.on('error', () => {report.aborted.errors++; clearTimeout(timer); resolve();});
    socket.once('data', bytes => {if(/^HTTP\/1\.[01] (429|503)\b/.test(bytes.toString('latin1'))) report.aborted.rejected++;});
    socket.on('connect', () => socket.write('POST /ui/config HTTP/1.1\r\nHost: ' + url.host + '\r\nContent-Type: application/json\r\nContent-Length: 256\r\n\r\n{'));
  });
}
let deadline;
try {
  const initial = await status();
  if(!['esp32c3', 'esp32s3', 'RP2350 Pico 2 W'].includes(initial.platform) && !(hostTest && initial.platform === 'host')) throw Error('Unexpected platform');
  const first = await metrics();
  if(first.stream_clients !== 0) throw Error('External SSE client already occupies a slot');
  report.initial = initial;
  await openStream(0);
  if(slow) await openSlowStream(); else await openStream(1);
  await pause(2500);
  const before = await metrics();
  if(before.stream_clients !== 2) throw Error('Two streams were not admitted');
  report.before = before; startedAt = Date.now(); deadline = startedAt + duration * 1000;
  console.log('LOAD_BEGIN ' + initial.platform + ' seconds=' + duration + ' slow=' + slow);
  // Runtime samples come from the existing SSE stream, not another polling client.
  const workers = [loop(apiMs, apiOnce), loop(pageMs, pageOnce)];
  if(overload) workers.push(loop(abortMs, abortPartial));
  await Promise.all(workers);
  const endedAt = Date.now();
  for(const index of slow ? [0] : [0, 1]) {
    const stream = report.streams[index];
    stream.max_gap_ms = Math.max(stream.max_gap_ms, endedAt - (stream.last_event_ms ?? startedAt));
    stream.tail_gap_ms = endedAt - (stream.last_event_ms ?? startedAt);
  }
  report.soak_streams = report.streams.map(value => ({...value}));
  report.after = await metrics();
  for(const field of ['tx_failures', 'tx_failed_jobs', 'tx_rejected_jobs'])
    if(typeof report.after[field] === 'number' && typeof before[field] === 'number')
      report[field + '_delta'] = report.after[field] - before[field];
  const elapsed = report.after.cpu_sample_us - before.cpu_sample_us;
  report.cpu_mean_percent = elapsed > 0 ? 100 * (report.after.cpu_busy_us - before.cpu_busy_us) / elapsed : null;
  report.http_request_delta = report.after.http_requests - before.http_requests;
  report.stream_record_delta = report.after.stream_records - before.stream_records;
  report.min_expected_events = Math.max(1, Math.floor(duration * 1000 * 0.8 / initial.interval_ms) - 2);
  report.max_accepted_gap_ms = Math.max(10000, initial.interval_ms * 5);
  report.api = {...report.api, ...percentiles(apiLatency)};
  report.page = {...report.page, ...percentiles(pageLatency)};
  // A paused local socket cannot prove whether a remote FIN is still buffered.
  report.slow_local_socket_open_at_end = slow ? !slowSocket.destroyed : undefined;
  save();
  if(overload) {
    // Let the bounded close path finish, then check that ordinary work recovers.
    await pause(3500);
    const pageCount = report.page.ok;
    await status();
    await pageOnce();
    const recovery = await metrics();
    report.recovery = {api: true, page: report.page.ok > pageCount, stream_clients: recovery.stream_clients};
    if(!report.recovery.page || recovery.stream_clients < (slow ? 1 : 2)) throw Error('Recovery failed');
  }
  if(otaPath) {
    if(statSync(otaPath).size > initial.max_firmware_bytes) throw Error('OTA image exceeds the reported limit');
    const image = readFileSync(otaPath), digest = createHash('sha256').update(image).digest('hex');
    const at = Date.now();
    report.soak_free_heap_sample_min = report.free_heap_min;
    report.soak_secondary_heap_sample_min = report.secondary_heap_min ?? null;
    inOta = true;
    const otaBeforeMetrics = await metrics();
    const otaStreamBefore = report.streams.map(value => value.events);
    const apiBefore = report.api.ok, apiErrorsBefore = report.api.errors, pageBefore = report.page.ok, pageErrorsBefore = report.page.errors;
    let otaRunning = true;
    const wake = new AbortController();
    const duringOta = async (intervalMs, action) => {
      while(otaRunning) {
        await action();
        if(otaRunning) await delay(intervalMs, undefined, {signal: wake.signal}).catch(() => {});
      }
    };
    const concurrent = [duringOta(apiMs, apiOnce)];
    if(overload) concurrent.push(duringOta(pageMs, pageOnce));
    let response, uploadMs, otaAfterMetrics, otaMetricsError;
    try {
      response = await fetch(base + '/ui/firmware', {method: 'POST', body: image,
        headers: {'Content-Type': 'application/octet-stream', 'X-Firmware-SHA256': digest, Connection: 'close'}, signal: AbortSignal.timeout(650000)});
      uploadMs = Date.now() - at;
      // End the API lane and drain a successful OTA response before requesting
      // metrics. Otherwise measurement itself can become a fifth connection.
      otaRunning = false; wake.abort();
      await Promise.allSettled(concurrent);
      if(response.status === 202) await response.text();
      try {otaAfterMetrics = await metrics();} catch(error) {otaMetricsError = String(error);}
    } finally {
      otaRunning = false; inOta = false; wake.abort(); stop = true;
      for(const controller of controllers) controller.abort();
      if(slowSocket) slowSocket.destroy();
      await Promise.allSettled(concurrent);
    }
    report.ota = {http_status: response.status, upload_ms: uploadMs, image_bytes: image.length};
    report.ota.api_ok = report.api.ok - apiBefore; report.ota.api_errors = report.api.errors - apiErrorsBefore;
    report.ota.page_ok = report.page.ok - pageBefore; report.ota.page_errors = report.page.errors - pageErrorsBefore;
    report.ota.sse_events = report.streams.map((value, index) => value.events - otaStreamBefore[index]);
    if(otaAfterMetrics && otaAfterMetrics.uptime_ms >= otaBeforeMetrics.uptime_ms && otaAfterMetrics.cpu_sample_us > otaBeforeMetrics.cpu_sample_us) {
      report.ota.cpu_mean_percent = 100 * (otaAfterMetrics.cpu_busy_us - otaBeforeMetrics.cpu_busy_us) /
        (otaAfterMetrics.cpu_sample_us - otaBeforeMetrics.cpu_sample_us);
      report.ota.lifetime_heap_before = otaBeforeMetrics.min_free_heap;
      report.ota.lifetime_heap_after = otaAfterMetrics.min_free_heap;
    } else {
      report.ota.cpu_mean_percent = null;
      report.ota.metrics_error = otaMetricsError ?? 'Missing CPU counter interval';
    }
    if(response.status !== 202) throw Error('OTA returned HTTP ' + response.status + await errorDetail(response));
    stop = true; for(const controller of controllers) controller.abort(); if(slowSocket) slowSocket.destroy();
    const until = Date.now() + 180000;
    while(Date.now() < until) {
      await pause(1000);
      try {
        const current = await status();
        if(current.running_slot === initial.running_slot) continue;
        report.ota.boot_slot = current.running_slot;
        report.ota.key_survived = true;
        report.ota.settings_survived = current.name === initial.name && current.interval_ms === initial.interval_ms && current.led === initial.led;
        if(!report.ota.settings_survived) throw Error('Settings changed during OTA');
        break;
      } catch(error) {if(String(error).includes('Settings changed')) throw error;}
    }
    if(!report.ota.boot_slot) throw Error('Updated slot did not return');
  }
  if(baselinePath) {
    const baseline = JSON.parse(readFileSync(baselinePath, 'utf8'));
    if(!baseline.pass || baseline.initial?.platform !== initial.platform || baseline.duration_seconds !== duration ||
      baseline.slow_second_stream !== slow || baseline.overload !== overload || baseline.initial?.interval_ms !== initial.interval_ms ||
      Boolean(baseline.ota) !== Boolean(otaPath) || baseline.api_interval_ms !== apiMs || baseline.page_interval_ms !== pageMs ||
      baseline.metrics_interval_ms !== metricsMs || baseline.abort_interval_ms !== abortMs)
      throw Error('Baseline is not a passing same-platform/same-workload report');
    report.difference_from_baseline = {free_heap_min: report.free_heap_min - baseline.free_heap_min,
      secondary_heap_min: report.secondary_heap_min != null && baseline.secondary_heap_min != null ? report.secondary_heap_min - baseline.secondary_heap_min : null,
      cpu_mean_percent: report.cpu_mean_percent != null && baseline.cpu_mean_percent != null ? report.cpu_mean_percent - baseline.cpu_mean_percent : null,
      ota_same_size: otaPath ? report.ota.image_bytes === baseline.ota.image_bytes : null};
  }
  const streamHealthy = value => value.errors === 0 && value.events >= report.min_expected_events && value.max_gap_ms <= report.max_accepted_gap_ms;
  report.api = {...report.api, ...percentiles(apiLatency)};
  report.page = {...report.page, ...percentiles(pageLatency)};
  report.pass = report.api.errors === 0 && report.page.errors === 0 && report.metrics_errors === 0 &&
    (overload || slow || report.tx_failed_jobs_delta == null || report.tx_failed_jobs_delta === 0) &&
    streamHealthy(report.soak_streams[0]) && (slow || streamHealthy(report.soak_streams[1])) &&
    (!otaPath || report.ota?.cpu_mean_percent != null);
  console.log('LOAD_END ' + JSON.stringify({pass: report.pass, api: report.api.ok, page: report.page.ok, streams: report.streams.map(value => value.events), cpu: report.cpu_mean_percent}));
} catch(error) {report.pass = false; problem('test', error); console.log('LOAD_FAILED ' + String(error));}
finally {
  stop = true;
  for(const controller of controllers) controller.abort();
  if(slowSocket) slowSocket.destroy();
  await Promise.allSettled(tasks);
  report.finished = new Date().toISOString(); save();
}
process.exitCode = report.pass ? 0 : 1;

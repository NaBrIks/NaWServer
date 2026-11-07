// Explicit hardware measurement: the OTA phase writes the inactive image slot.
// No settings or API-key operations. Run boards sequentially on the same network.
import {readFileSync, writeFileSync, readdirSync, existsSync} from 'node:fs';
import {resolve} from 'node:path';
import {createHash} from 'node:crypto';
import {chromium} from 'playwright';

const [base, imageFile, reportFile, secondsText = '20'] = process.argv.slice(2);
if(!reportFile) throw Error('Usage: metrics-scenarios.mjs BASE IMAGE PRIVATE_REPORT [seconds=20]');
const seconds = Number(secondsText);
if(!Number.isFinite(seconds) || seconds < 10 || seconds > 60) throw Error('Use a 10..60 second window');
const image = readFileSync(imageFile);
const digest = createHash('sha256').update(image).digest('hex');
const report = {started: new Date().toISOString(), base, image_bytes: image.length, image_sha256: digest, seconds, phases: []};
const monitor = process.env.NAWSERVER_DEMO_MONITOR_URL;
const save = () => writeFileSync(reportFile, JSON.stringify(report, null, 2) + '\n');
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function quietStart() {
  // The local catalog probes registered devices every 30 seconds. Keep it
  // running and place short windows between probes instead of hiding its load.
  if(!monitor || seconds > 20) return;
  const deadline = Date.now() + 35000;
  while(Date.now() < deadline) {
    let links;
    try {
      const response = await fetch(new URL('/api/links', monitor), {signal: AbortSignal.timeout(2000)});
      links = await response.json();
    } catch {return;}
    const entry = links.find(value => value.url?.replace(/\/$/, '') === base.replace(/\/$/, ''));
    if(!entry?.status?.checkedAt) return;
    const age = Date.now() - Date.parse(entry.status.checkedAt);
    if(age >= 5000 && age <= 7000) {report.monitor_alignment = 'between local status monitor 30-second probes'; return;}
    await pause(500);
  }
  throw Error('Could not align a quiet interval between catalog probes');
}
async function metrics() {
  const response = await fetch(base + '/ui/metrics', {headers: {Connection: 'close'}, signal: AbortSignal.timeout(4000)});
  if(!response.ok) throw Error('Metrics HTTP ' + response.status);
  return response.json();
}
const summary = (before, after, samples = []) => {
  if(after.uptime_ms < before.uptime_ms) throw Error('Unexpected reboot inside measurement window');
  const busy = after.cpu_busy_us - before.cpu_busy_us;
  const total = after.cpu_sample_us - before.cpu_sample_us;
  const records = samples.filter(value => value.uptime_ms >= before.uptime_ms && value.uptime_ms <= after.uptime_ms);
  const values = [before, ...records, after];
  return {
    cpu_mean_percent: total > 0 ? 100 * busy / total : null,
    cpu_measured_core_us: total,
    cpu_cores: after.cpu_cores,
    wall_ms: after.uptime_ms - before.uptime_ms,
    heap_scope: after.heap_scope, heap_total: after.total_heap,
    heap_free_start: before.free_heap, heap_free_end: after.free_heap,
    heap_free_sample_min: Math.min(...values.map(value => value.free_heap)),
    heap_lifetime_min_start: before.min_free_heap, heap_lifetime_min_end: after.min_free_heap,
    largest_free_sample_min: Math.min(...values.map(value => value.largest_free_block)),
    secondary_heap_total: after.secondary_heap_total ?? 0,
    secondary_heap_free_sample_min: after.secondary_heap_total ? Math.min(...values.map(value => value.secondary_heap_free)) : null,
    http_requests: after.http_requests - before.http_requests,
    stream_records: after.stream_records - before.stream_records,
    samples: records.length,
    before, after
  };
};
let browser;
try {
  await quietStart();
  let initial = await metrics();
  if(!initial.cpu_available || initial.stream_clients) throw Error('Need a ready CPU sampler and no existing stream clients');
  report.initial = initial;
  console.log('IDLE_BEGIN');
  const idleBefore = initial;
  await pause(seconds * 1000);
  const idleAfter = await metrics();
  const idle = summary(idleBefore, idleAfter);
  idle.clean = idle.http_requests === 1 && idle.stream_records === 0 && idleAfter.stream_clients === 0;
  report.phases.push({name: 'idle', ...idle}); save();
  if(!idle.clean) throw Error('Idle contaminated by another client; inspect report');
  console.log('IDLE_PASS cpu=' + idle.cpu_mean_percent.toFixed(3));

  const root = resolve(process.env.LOCALAPPDATA ?? '', 'ms-playwright');
  const executable = process.env.NAWSERVER_BROWSER_EXECUTABLE ?? readdirSync(root)
    .filter(name => name.startsWith('chromium_headless_shell-'))
    .map(name => resolve(root, name, 'chrome-win/headless_shell.exe')).find(existsSync);
  browser = await chromium.launch({headless: true, executablePath: executable});
  const page = await browser.newPage();
  await page.addInitScript(() => {
    window.__statusSamples = [];
    const parse = JSON.parse;
    JSON.parse = function(...args) {
      const value = parse(...args);
      if(value && typeof value.free_heap === 'number') window.__statusSamples.push(value);
      return value;
    };
  });
  await page.goto(base + '/', {waitUntil: 'domcontentloaded'});
  await page.waitForFunction(() => document.querySelector('#link')?.textContent === 'Подключено' && window.__statusSamples.length > 0);
  await pause(2500);
  await quietStart();
  const pageBefore = await metrics();
  if(pageBefore.stream_clients !== 1) throw Error('Expected exactly the measured page stream');
  console.log('PAGE_BEGIN interval_ms=' + pageBefore.interval_ms);
  await pause(seconds * 1000);
  const pageAfter = await metrics();
  const pageStats = summary(pageBefore, pageAfter, await page.evaluate(() => window.__statusSamples));
  pageStats.clean = pageStats.http_requests === 1 && pageAfter.stream_clients === 1;
  report.phases.push({name: 'page', ...pageStats}); save();
  if(!pageStats.clean) throw Error('Page window contaminated by other HTTP requests');
  console.log('PAGE_PASS cpu=' + pageStats.cpu_mean_percent.toFixed(3));

  // One real page stays open during OTA, matching upload from the management UI.
  await quietStart();
  const otaBefore = await metrics();
  console.log('OTA_BEGIN');
  const began = Date.now();
  const response = await fetch(base + '/ui/firmware', {method: 'POST',
    headers: {'Content-Type': 'application/octet-stream', 'X-Firmware-SHA256': digest, Connection: 'close'},
    body: image, signal: AbortSignal.timeout(650000)});
  const reply = await response.text();
  if(response.status !== 202) throw Error('OTA HTTP ' + response.status + ': ' + reply);
  const wireMs = Date.now() - began;
  const otaAfter = await metrics();
  const ota = summary(otaBefore, otaAfter, await page.evaluate(() => window.__statusSamples));
  ota.upload_ms = wireMs; ota.http_status = response.status;
  report.phases.push({name: 'ota_with_page', ...ota}); save();
  await browser.close(); browser = undefined;

  const bootDeadline = Date.now() + 180000;
  let boot;
  while(Date.now() < bootDeadline) {
    await pause(1000);
    try {const value = await metrics(); if(value.running_slot !== otaBefore.running_slot) {boot = value; break;}}
    catch {}
  }
  if(!boot) throw Error('Updated slot did not become available');
  if(boot.name !== initial.name || boot.interval_ms !== initial.interval_ms || boot.led !== initial.led) throw Error('Settings changed');
  report.boot = boot;
  report.pass = true;
  console.log('OTA_PASS upload_ms=' + wireMs + ' cpu=' + ota.cpu_mean_percent.toFixed(3));
} catch(error) {
  report.pass = false; report.error = String(error); console.log('FAIL ' + report.error);
} finally {
  if(browser) await browser.close();
  report.finished = new Date().toISOString(); save();
}
process.exitCode = report.pass ? 0 : 1;

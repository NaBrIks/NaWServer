import {spawn} from 'node:child_process';
import {mkdtempSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {createServer} from 'node:net';

// Runtime credentials remain in memory. The isolated key store stays in the OS
// temporary directory, outside the checkout, and is never included in reports.
const args = process.argv.slice(2);
const useWsl = args[0] === '--wsl';
if(useWsl) args.shift();
if(!args[0]) throw Error('Usage: node host-accept.mjs [--wsl] executable');
const executable = resolve(args[0]);
const state = mkdtempSync(join(tmpdir(), 'nawserver-compact-'));
const linuxPath = value => value.replaceAll('\\', '/').replace(/^([A-Za-z]):\//, (_, drive) => '/mnt/' + drive.toLowerCase() + '/');
const probe = createServer();
await new Promise((resolve, reject) => {probe.once('error', reject); probe.listen(0, '127.0.0.1', resolve);});
const port = probe.address().port;
await new Promise(resolve => probe.close(resolve));
const command = useWsl ? 'wsl.exe' : executable;
const childArgs = useWsl
  ? ['-d', 'Ubuntu', '--exec', linuxPath(executable), String(port), '15', linuxPath(state)]
  : [String(port), '15', state];
const child = spawn(command, childArgs, {stdio: ['ignore', 'pipe', 'pipe']});
let output = '', errors = '', key = '', ready = false;
child.stdout.on('data', bytes => {
  output += bytes;
  key = output.match(/NEW_KEY=([a-f0-9]{64})/)?.[1] || key;
  ready = output.includes('READY');
});
child.stderr.on('data', bytes => {errors += bytes;});
const completion = new Promise((resolve, reject) => {child.once('error', reject); child.once('exit', resolve);});
const report = {checks: []};
const check = (value, label) => {if(!value) throw Error(label); report.checks.push(label);};
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
try {
  const deadline = Date.now() + 10000;
  while(!ready && Date.now() < deadline) await pause(50);
  check(ready && key, 'start and provision');
  const base = 'http://127.0.0.1:' + port;
  let auth = {'X-Client-Key': key};
  const request = (path, options = {}) => fetch(base + path, {...options, signal: options.signal || AbortSignal.timeout(5000)});
  let response = await request('/');
  check(response.ok && (await response.text()).includes('Устройство'), 'gzip static page');
  check((await request('/api/status')).status === 401, 'early unauthorized rejection');
  check((await request('/ui/status')).status === 200, 'management UI needs no API credential');
  check((await request('/ui/api-key', {method: 'POST', headers: {'Content-Type': 'application/json', Origin: 'http://other.invalid'}, body: '{}'})).status === 403,
    'foreign origin cannot rotate key');
  check((await request('/api/status', {headers: {...auth, Origin: 'http://other.invalid'}})).status === 403, 'foreign origin rejected');
  response = await request('/api/status', {headers: auth});
  check(response.ok && (await response.json()).interval_ms === 2000, 'initial settings');
  response = await request('/api/config', {method: 'POST', headers: {...auth, 'Content-Type': 'application/json'},
    body: JSON.stringify({name: 'Tiny device', interval_ms: 1000, led: true})});
  check(response.ok, 'save settings');
  const applied = await response.json();
  check(applied.name === 'Tiny device' && applied.led && applied.interval_ms === 1000, 'settings applied');
  check((await request('/api/config', {method: 'POST', headers: {...auth, 'Content-Type': 'application/json'},
    body: '{"name":"a","name":"b","interval_ms":1000,"led":true}'})).status === 400, 'duplicate settings rejected');
  const abort = new AbortController();
  response = await request('/api/events', {headers: auth, signal: abort.signal});
  check(response.ok, 'status stream opened');
  const reader = response.body.getReader();
  let frames = '';
  const timer = setTimeout(() => abort.abort(), 8000);
  try {
    while(frames.split('\n\n').length < 3) {
      const part = await reader.read();
      if(part.done) break;
      frames += new TextDecoder().decode(part.value);
    }
    check(frames.includes('Tiny device') && frames.includes('data:'), 'status stream values');
    const parallel = new AbortController();
    const second = await request('/ui/events', {signal: parallel.signal});
    check(second.ok, 'second status stream accepted');
    const secondReader = second.body.getReader();
    const firstFrame = await secondReader.read();
    check(!firstFrame.done && new TextDecoder().decode(firstFrame.value).includes('Tiny device'), 'second status stream receives values');
    check((await request('/api/events', {headers: auth})).status === 503, 'third status stream rejected');
    response = await request('/');
    check(response.ok && (await response.text()).includes('Устройство'), 'HTTP works alongside SSE');
    response = await request('/ui/api-key', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'});
    check(response.status === 201 && response.headers.get('cache-control') === 'no-store', 'explicit web key creation');
    const issued = await response.json();
    check(/^[a-f0-9]{64}$/.test(issued.key) && issued.key !== key, 'new automation credential returned');
    check((await request('/api/status', {headers: auth})).status === 401, 'old automation credential revoked');
    key = issued.key; auth = {'X-Client-Key': key};
    check((await request('/api/status', {headers: auth})).status === 200, 'new automation credential accepted');
    let ended = false;
    while(!ended) {const part = await reader.read(); ended = part.done;}
    check(ended, 'old authenticated stream closed on rotation');
    check(!(await secondReader.read()).done, 'public second stream survives key rotation');
    parallel.abort();
  } finally {clearTimeout(timer); abort.abort();}
  const uiAbort = new AbortController();
  response = await request('/ui/events', {signal: uiAbort.signal});
  check(response.ok, 'management stream opens without API credential');
  const uiReader = response.body.getReader();
  const uiTimer = setTimeout(() => uiAbort.abort(), 3000);
  try {
    response = await request('/ui/api-key', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'});
    check(response.status === 201, 'management can explicitly replace key');
    key = (await response.json()).key; auth = {'X-Client-Key': key};
    let records = '';
    while(records.split('\n\n').length < 3) {
      const part = await uiReader.read();
      check(!part.done, 'management stream survives automation key rotation');
      records += new TextDecoder().decode(part.value);
    }
  } finally {clearTimeout(uiTimer); uiAbort.abort();}
  check((await request('/api/firmware', {method: 'POST', headers: {...auth, 'Content-Type': 'application/octet-stream'},
    body: 'not-firmware'})).status === 501, 'host OTA explicitly unsupported');
  let shutdownTimer;
  const shutdownDeadline = new Promise((_, reject) => {
    shutdownTimer = setTimeout(() => reject(Error('shutdown timeout')), 12000);
  });
  let exit;
  try {exit = await Promise.race([completion, shutdownDeadline]);}
  finally {clearTimeout(shutdownTimer);}
  check(exit === 0, 'graceful shutdown releases transport');
  report.pass = true;
} catch(error) {
  report.pass = false;
  report.error = String(error);
  report.stderr = errors.replaceAll(key || '<none>', '<redacted>');
  report.stdout = output.replace(/NEW_KEY=[a-f0-9]+/g, 'NEW_KEY=<redacted>');
  child.kill();
}
console.log(JSON.stringify(report));
process.exitCode = report.pass ? 0 : 1;

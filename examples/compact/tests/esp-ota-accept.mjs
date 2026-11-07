// Explicit hardware acceptance. The credential is read from a private file, never printed.
import {readFileSync,writeFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import net from 'node:net';
const [base,keyFile,aFile,bFile,reportFile]=process.argv.slice(2);
if(!reportFile)throw Error('Usage: esp-ota-accept.mjs BASE KEY_FILE A_BIN B_BIN REPORT_FILE');
const key=readFileSync(keyFile,'utf8').trim(),a=readFileSync(aFile),b=readFileSync(bFile);
const report={started:new Date().toISOString(),updates:[],negative:[],errors:[]};
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
async function status(){const r=await fetch(base+'/api/status',{headers:{'X-Client-Key':key,Connection:'close'},signal:AbortSignal.timeout(4000)});if(r.status!==200)throw Error('Status '+r.status);return r.json();}
async function waitBoot(previous,version){let last;for(let i=0;i<90;i++){await sleep(750);try{const now=await status();last=now;if(now.running_slot!==previous.running_slot&&(!version||now.firmware===version)){await sleep(5500);return status();}}catch{}}throw Error('Boot not observed: '+JSON.stringify(last));}
async function update(bytes,label,previous,version){const at=Date.now(),hash=sha(bytes);const r=await fetch(base+'/api/firmware',{method:'POST',headers:{'X-Client-Key':key,'X-Firmware-SHA256':hash,'Content-Type':'application/octet-stream',Connection:'close'},body:bytes,signal:AbortSignal.timeout(120000)});const text=await r.text();if(r.status!==202)throw Error('OTA '+label+' HTTP'+r.status+' '+text);const after=await waitBoot(previous,version);const item={label,bytes:bytes.length,sha256:hash,ms:Date.now()-at,before:previous,after};report.updates.push(item);if(after.name!==previous.name||after.interval_ms!==previous.interval_ms||after.led!==previous.led)throw Error('Settings did not survive '+label);writeFileSync(reportFile,JSON.stringify(report,null,2));return after;}
try{
 report.before=await status();await sleep(5500);
 // Correct image magic with deliberately incorrect digest: no boot selection change.
 let at=Date.now(),result={kind:'bad_hash'};try{const r=await fetch(base+'/api/firmware',{method:'POST',headers:{'X-Client-Key':key,'X-Firmware-SHA256':'0'.repeat(64),'Content-Type':'application/octet-stream',Connection:'close'},body:b,signal:AbortSignal.timeout(120000)});result.status=r.status;result.reply=await r.text();if(r.status<400)throw Error('Bad digest accepted');}catch(e){if(String(e).includes('Bad digest accepted'))throw e;result.rejectedConnection=true;result.error=String(e);}result.ms=Date.now()-at;result.after=await status();if(result.after.running_slot!==report.before.running_slot||result.after.uptime_ms<report.before.uptime_ms)throw Error('Bad digest changed boot');report.negative.push(result);
 const url=new URL(base);await new Promise((resolve,reject)=>{const socket=net.connect(Number(url.port)||80,url.hostname);socket.on('error',reject);socket.on('connect',()=>{socket.write('POST /api/firmware HTTP/1.1\r\nHost: '+url.host+'\r\nX-Client-Key: '+key+'\r\nContent-Type: application/octet-stream\r\nX-Firmware-SHA256: '+sha(b)+'\r\nContent-Length: '+b.length+'\r\nConnection: close\r\n\r\n');socket.write(b.subarray(0,8192));setTimeout(()=>{socket.destroy();resolve();},500);});});await sleep(2000);const afterPartial=await status();if(afterPartial.running_slot!==report.before.running_slot||afterPartial.uptime_ms<report.before.uptime_ms)throw Error('Truncation changed boot');report.negative.push({kind:'truncated',sent:8192,after:afterPartial});
 const middle=await update(b,'A-to-B',afterPartial);if(middle.firmware===report.before.firmware)throw Error('B image must have a distinct version');
 report.after=await update(a,'B-to-A',middle,report.before.firmware);if(report.after.running_slot!==report.before.running_slot)throw Error('A slot not restored');report.keyPersisted=true;report.pass=true;
}catch(e){report.pass=false;report.errors.push(String(e));try{report.last=await status();}catch{}}
report.finished=new Date().toISOString();writeFileSync(reportFile,JSON.stringify(report,null,2));console.log(JSON.stringify(report));process.exitCode=report.pass?0:1;

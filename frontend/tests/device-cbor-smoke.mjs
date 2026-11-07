// Explicit target only. Reads telemetry/assets and sends harmless WS echo probes.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {decode,encode} from 'cborg';
import {decodeWire,encodeWire} from '../src/contracts/cbor.ts';
const arg=name=>{const i=process.argv.indexOf(name);return i<0?undefined:process.argv[i+1]};
const target=arg('--url');if(!target)throw Error('--url http://device is required');
const base=new URL(target);if(base.protocol!=='http:'||base.username||base.password)throw Error('Explicit HTTP target required');
let passed=0;
function check(ok,label){assert(ok,label);console.log('PASS '+label);passed++;}
function textKeys(value){if(value instanceof Map){for(const [key,item] of value){assert.equal(typeof key,'string');textKeys(item)}}else if(Array.isArray(value))value.forEach(textKeys)}
async function get(path){
 const response=await fetch(new URL(path,base),{signal:AbortSignal.timeout(10000)});
 check(response.ok&&response.headers.get('content-type')?.startsWith('application/cbor'),path+' CBOR');
 const bytes=new Uint8Array(await response.arrayBuffer());textKeys(decode(bytes,{useMaps:true}));
 check(bytes.length<=4096,path+' text map keys; '+bytes.length+' B <= 4096 B');return decodeWire(bytes);
}
const info=await get('/api/v2/info');check(info.v===2&&info.source==='device'&&info.chip.model===arg('--model'),'device identity');
const summary=await get('/api/v2/summary');check(summary.data.memory[0].free_bytes>0,'live memory');
const tasks=await get('/api/v2/tasks?limit=8');check(tasks.data.items.length>0&&tasks.data.items.every(t=>typeof t.name==='string'),'FreeRTOS tasks');
if(info.chip.model==='ESP32-S3'){const network=await get('/api/v2/network');check(network.ready&&summary.data.display.ready,'network and LCD ready')}
if(arg('--manifest')){
 const manifest=JSON.parse(await readFile(arg('--manifest'),'utf8'));
 for(const asset of manifest.files){
  const response=await fetch(new URL(asset.path==='index.html'?'/':'/'+asset.path,base),{signal:AbortSignal.timeout(15000)});
  assert(response.ok);const bytes=new Uint8Array(await response.arrayBuffer());
  check(createHash('sha256').update(bytes).digest('hex')===asset.sha256,'asset SHA256 '+asset.path);
 }
}
const socketUrl=new URL('/ws',base);socketUrl.protocol='ws:';
const ws=new WebSocket(socketUrl);ws.binaryType='arraybuffer';
const replies=[],waiters=[];let gotSummary=false;
ws.addEventListener('close',event=>{for(const waiter of waiters.splice(0))waiter.reject(Error('WS closed '+event.code+' '+event.reason))});
ws.addEventListener('message',event=>{
 try{assert(event.data instanceof ArrayBuffer);const bytes=new Uint8Array(event.data);textKeys(decode(bytes,{useMaps:true}));const value=decodeWire(bytes);
  if(value.type==='summary'){assert(bytes.length<=4096);gotSummary=value.source==='device';return}
  if(waiters.length)waiters.shift().resolve(value);else replies.push(value);
 }catch(error){for(const waiter of waiters.splice(0))waiter.reject(error)}
});
async function request(bytes){
 const response=new Promise((resolve,reject)=>{if(replies.length)return resolve(replies.shift());const waiter={resolve,reject};waiters.push(waiter);const timer=setTimeout(()=>reject(Error('WS response timeout')),8000);waiter.resolve=value=>{clearTimeout(timer);resolve(value)};waiter.reject=error=>{clearTimeout(timer);reject(error)};});ws.send(bytes);return response;
}
try{
 await new Promise((resolve,reject)=>{ws.addEventListener('open',resolve,{once:true});ws.addEventListener('error',reject,{once:true});setTimeout(()=>reject(Error('WS open timeout')),8000).unref()});
 const payload=Uint8Array.from({length:3800},(_,i)=>i%256);
 const reply=await request(encodeWire({v:2,type:'echo.request',request_id:'text-keys',future_extension:{'температура':23.5,counter:18446744073709551615n,samples:[null,true,new Uint8Array([0,255])]},data:{bytes:payload}}));
 check(reply.type==='echo.response'&&reply.request_id==='text-keys'&&Buffer.from(reply.data.bytes).equals(Buffer.from(payload)),'3800-byte binary/NUL echo; unknown nested UTF-8 fields and native values accepted');
 const text='Имена полей напрямую ✓';const response=await request(encodeWire({v:2,type:'echo.request',request_id:'unicode',data:{text}}));
 check(response.data.text===text,'UTF-8 echo');
 for(const [label,bytes] of [
  ['numeric map key',encode(new Map([[1,2]]))],
  ['duplicate map key',new Uint8Array([0xa2,0x61,0x76,2,0x61,0x76,2])],
  ['invalid UTF-8 key',new Uint8Array([0xa1,0x61,0xff,0])],
  ['trailing item',new Uint8Array([0xa0,0])]
 ]){const rejected=await request(bytes);check(rejected.type==='error','reject '+label)}
 if(!gotSummary)await new Promise(resolve=>setTimeout(resolve,1200));check(gotSummary,'binary WS summary with direct text keys');
}finally{ws.close()}
const controller=new AbortController();const timer=setTimeout(()=>controller.abort(),8000);
try{
 const response=await fetch(new URL('/api/v2/events',base),{signal:controller.signal});assert(response.ok);const reader=response.body.getReader();let text='';
 while(!text.includes('event: diagnostic')){const part=await reader.read();if(part.done)throw Error('SSE ended');text+=new TextDecoder().decode(part.value)}
 check(true,'SSE diagnostic stream');
}finally{clearTimeout(timer);controller.abort()}
console.log('RESULT '+passed+' device protocol/asset checks passed');

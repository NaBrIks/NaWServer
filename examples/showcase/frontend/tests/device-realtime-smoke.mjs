// Read-only protocol acceptance for the explicitly selected Showcase C3.
// No flash, load commands, LED or storage writes. Raw evidence belongs in ignored local/.
import {readFile,writeFile} from 'node:fs/promises';
import {encode,decode} from 'cborg';
const at=process.argv.indexOf('--url');if(at<0||!process.argv[at+1])throw Error('Use --url http://CURRENT_DEVICE_IP [--backend tcp|httpd] [--out PATH]');
const base=new URL(process.argv[at+1]);if(base.protocol!=='http:'||base.username||base.password||!/^\d+\.\d+\.\d+\.\d+$/.test(base.hostname))throw Error('An explicit numeric HTTP device address is required');
const backendIndex=process.argv.indexOf('--backend'),backend=backendIndex<0?'httpd':process.argv[backendIndex+1];
if(!['tcp','httpd'].includes(backend))throw Error('--backend must be tcp or httpd');
// Match the selected backend's actual task, rather than accepting an unrelated worker.
const httpTaskName=backend==='tcp'?'naw_tcp_http':'httpd';
const outIndex=process.argv.indexOf('--out'),output=outIndex>=0?process.argv[outIndex+1]:null;
const pack=v=>v instanceof Uint8Array||v===null||typeof v!=='object'?v:Array.isArray(v)?v.map(pack):new Map(Object.entries(v).map(([k,x])=>[k,pack(x)]));
const unpack=v=>v instanceof Map?Object.fromEntries([...v].map(([k,x])=>[k,unpack(x)])):Array.isArray(v)?v.map(unpack):v;
const parse=bytes=>unpack(decode(bytes,{useMaps:true,allowIndefinite:false,rejectDuplicateMapKeys:true}));
const report={result:'RUNNING',backend,httpTaskName,checks:[],counts:{ws:0,echo:0,sse:0,privateSse:0,http:0},errors:[],samples:[],url:base.origin};
function check(ok,label){if(!ok)throw Error(label);report.checks.push(label);console.log('PASS '+label);}
const auth='Basic '+Buffer.from('demo:showcase-only').toString('base64');
async function get(path,headers={}){const r=await fetch(new URL(path,base),{headers,signal:AbortSignal.timeout(6000)});return {response:r,bytes:new Uint8Array(await r.arrayBuffer())};}
async function cbor(path){const {response,bytes}=await get(path);if(!response.ok||!response.headers.get('content-type')?.startsWith('application/cbor'))throw Error('CBOR request failed: '+path+' / '+response.status);return parse(bytes);}
let ws;const controllers=[];const tasks=[];let stopping=false;
async function sse(path,privateStream=false){const controller=new AbortController();controllers.push(controller);const r=await fetch(new URL(path,base),{headers:privateStream?{Authorization:auth}:{},signal:controller.signal});if(!r.ok||!r.headers.get('content-type')?.startsWith('text/event-stream'))throw Error('SSE rejected '+r.status);const reader=r.body.getReader();const decoder=new TextDecoder();let buffer='';
 try{while(!stopping){const part=await reader.read();if(part.done){if(!stopping)throw Error('SSE ended unexpectedly');break;}buffer+=decoder.decode(part.value,{stream:true});if(buffer.length>8192)throw Error('SSE framing bound');let end;while((end=buffer.indexOf('\n\n'))>=0){const event=buffer.slice(0,end).replaceAll('\r','');buffer=buffer.slice(end+2);if(!event.includes('event: diagnostic'))continue;const data=event.split('\n').filter(x=>x.startsWith('data:')).map(x=>x.slice(5).trimStart()).join('\n');const sample=JSON.parse(data);if(sample.v!==2||sample.source!=='device'||sample.boot_id!==report.boot_id||!Number.isFinite(sample.free_bytes)||sample.free_bytes<=0)throw Error('Invalid or different-boot SSE sample');report.counts[privateStream?'privateSse':'sse']++;if(!privateStream){if(report.samples.length&&sample.captured_ms<=report.samples.at(-1).captured_ms)throw Error('SSE time did not advance');report.samples.push(sample);}}}}
 finally{await reader.cancel().catch(()=>{});controller.abort();}
}
try{
 const info=await cbor('/api/v2/info');report.boot_id=info.boot_id;report.firmware=info.firmware;
 check(info.source==='device'&&info.chip.model==='ESP32-C3'&&info.chip.cores===1&&typeof info.firmware?.idf_version==='string'&&info.firmware.idf_version.trim().length>0&&info.firmware.http_backend===(backend==='tcp'?'tcp':'idf_httpd'),'actual C3 source / chip / SDK / HTTP backend');
 check(info.capabilities.sse.supported&&!info.capabilities.simulator_control.supported,'SSE enabled / no simulator capability');
 const start=await cbor('/api/v2/summary');check(start.source==='device'&&start.boot_id===info.boot_id&&start.data.memory[0].free_bytes>0,'real heap / shared boot');report.before=start;
 for(const probe of [{path:'/api/v2/auth/status',expected:401,header:null},{path:'/api/v2/auth/status',expected:401,header:'Basic '+Buffer.from('demo:wrong').toString('base64')},{path:'/api/v2/auth/status',expected:200,header:auth},{path:'/api/v2/auth/admin',expected:403,header:auth},{path:'/api/v2/events/private',expected:401,header:null}]){const {response}=await get(probe.path,probe.header?{Authorization:probe.header}:{});check(response.status===probe.expected,probe.path+' status '+probe.expected);if(probe.expected===401)check(response.headers.get('www-authenticate')?.startsWith('Basic'),'Basic challenge before response/stream');}
 const socketUrl=new URL('/ws',base);socketUrl.protocol='ws:';ws=new WebSocket(socketUrl);ws.binaryType='arraybuffer';
 await new Promise((resolve,reject)=>{const timeout=setTimeout(()=>reject(Error('WS open timeout')),6000);ws.onopen=()=>{clearTimeout(timeout);resolve();};ws.onerror=()=>{clearTimeout(timeout);reject(Error('WS connection failed'));};});
 ws.onmessage=event=>{try{if(!(event.data instanceof ArrayBuffer))throw Error('Nonbinary WS');const d=parse(new Uint8Array(event.data));if(d.type==='summary'){if(d.source!=='device'||d.boot_id!==info.boot_id)throw Error('WS device identity');report.counts.ws++;}else if(d.type==='echo.response'){if(!Buffer.from(d.data.bytes).equals(Buffer.from([0,128,255,0])))throw Error('WS echo bytes');report.counts.echo++;}}catch(error){report.errors.push(String(error));}};
 ws.onerror=()=>report.errors.push('WS transport error');ws.onclose=()=>{if(!stopping)report.errors.push('WS closed during mixed test');};
 for(const [path,isPrivate] of [['/api/v2/events',false],['/api/v2/events/private',true]])tasks.push(sse(path,isPrivate).catch(error=>{if(!stopping)report.errors.push(String(error));}));
 const started=Date.now();
 for(let tick=0;tick<30;tick++){
  ws.send(encode(pack({v:2,type:'echo.request',request_id:'mixed-'+tick,data:{bytes:new Uint8Array([0,128,255,0])}})));
  const summary=await cbor('/api/v2/summary');if(summary.boot_id!==info.boot_id)throw Error('Device rebooted');report.counts.http++;
  if(report.errors.length)throw Error(report.errors.join('; '));
  await new Promise(resolve=>setTimeout(resolve,Math.max(0,started+(tick+1)*1000-Date.now())));
 }
 check(report.counts.ws>=20&&report.counts.echo>=25,'WS summary and binary/NUL echo while both SSE streams active');
 check(report.counts.sse>=20&&report.counts.privateSse>=20,'public and authenticated SSE concurrently for 30 s');
 check(report.counts.http===30&&report.errors.length===0,'HTTP responsive alongside WS + two SSE streams');
 report.after=await cbor('/api/v2/summary');check(report.after.boot_id===info.boot_id&&report.after.seq>start.seq&&report.after.data.uptime_ms>start.data.uptime_ms,'no reboot / advancing real snapshots');
 let allTasks=[];
 for(let attempt=0;attempt<3;attempt++){
  let offset=0,snapshot='',restart=false;allTasks=[];
  for(let page=0;page<8;page++){
   const path=`/api/v2/tasks?offset=${offset}&limit=8${snapshot?'&snapshot_id='+encodeURIComponent(snapshot):''}`;
   const {response,bytes}=await get(path);if(response.status===409){restart=true;break;}if(!response.ok)throw Error('Task page '+response.status);
   const value=parse(bytes);snapshot=value.data.snapshot_id;allTasks.push(...value.data.items);
   if(value.data.next_offset===null)break;offset=value.data.next_offset;
  }
  if(!restart)break;
 }
 report.tasks=allTasks;check(allTasks.some(t=>t.name===httpTaskName)&&allTasks.every(t=>t.stack_min_free_bytes>0),'real FreeRTOS tasks / stack margins');
 report.elapsed_ms=Date.now()-started;report.result='PASS';
} catch(error){report.result='FAIL';report.errors.push(String(error));process.exitCode=1;console.error(String(error));}
finally{stopping=true;for(const controller of controllers)controller.abort();ws?.close();await Promise.allSettled(tasks);if(output)await writeFile(output,JSON.stringify(report,(_,v)=>typeof v==='bigint'?v.toString():v,2));console.log(JSON.stringify({result:report.result,checks:report.checks.length,counts:report.counts,errors:report.errors},null,2));}

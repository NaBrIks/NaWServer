import net from 'node:net';
import {decode} from 'cborg';
import {readFile,mkdir,writeFile} from 'node:fs/promises';
if(!process.env.LCD7_BASE_URL)throw Error('Set LCD7_BASE_URL to the test device origin');
const base=process.env.LCD7_BASE_URL,url=new URL(base);
const unpack=m=>Object.fromEntries([...m].map(([k,v])=>[k,v]));
const output=new URL('../../local/admission/',import.meta.url);await mkdir(output,{recursive:true});const report={checks:[]},held=[],websockets=[],streams=[];
const check=(ok,name)=>{if(!ok)throw Error(name);report.checks.push(name);console.log('PASS '+name)};
const network=async()=>{const r=await fetch(base+'/api/v2/network',{signal:AbortSignal.timeout(5000)});check(r.status===200,'diagnostic endpoint available');return unpack(decode(new Uint8Array(await r.arrayBuffer()),{useMaps:true}))};
const wait=ms=>new Promise(r=>setTimeout(r,ms));
async function socket(){return new Promise((resolve,reject)=>{const ws=new WebSocket(base.replace(/^http/,'ws')+'/ws');websockets.push(ws);ws.addEventListener('open',()=>resolve(ws),{once:true});ws.addEventListener('error',reject,{once:true});setTimeout(()=>reject(Error('WS timeout')),5000).unref()})}
async function oneHttp(){return new Promise((resolve,reject)=>{const s=net.createConnection({host:url.hostname,port:Number(url.port)||80});held.push(s);let bytes=Buffer.alloc(0),done=false;const timer=setTimeout(()=>{if(!done){report.timeout={connected:!s.connecting,state:s.readyState,bytes:bytes.toString()};reject(Error('HTTP admission timeout'))}},5000);timer.unref();s.on('error',reject);s.on('connect',()=>s.write('GET /health HTTP/1.1\r\nHost: '+url.host+'\r\nConnection: keep-alive\r\n\r\n'));s.on('data',chunk=>{bytes=Buffer.concat([bytes,chunk]);const end=bytes.indexOf('\r\n\r\n');if(end<0||done)return;const headers=bytes.subarray(0,end).toString(),length=Number(headers.match(/content-length:\s*(\d+)/i)?.[1]||0);if(bytes.length<end+4+length)return;done=true;clearTimeout(timer);resolve({status:Number(headers.slice(9,12)),headers,body:bytes.subarray(end+4,end+4+length).toString()})});})}
try{
 report.baseline=await network();check(report.baseline.httpd_limit===21&&report.baseline.http_reserve===3&&report.baseline.socket_limit===24&&report.baseline.tcp_pcb_limit===32,'live configured limits and reserve');
 let state=await network();for(let n=0;n<4&&state.ws_clients<state.ws_limit;n++){await socket();await wait(100);state=await network()}
 check(state.ws_clients===state.ws_limit,'WS reaches declared limit including owner page');
 const admission=await fetch(base+'/api/v2/admission?kind=ws');const refusal=await admission.json();check(admission.status===503&&refusal.reason==='ws_limit'&&admission.headers.get('retry-after')==='2','HTTP preflight explains WS limit with Retry-After');
 const denied=new WebSocket(base.replace(/^http/,'ws')+'/ws');websockets.push(denied);const closed=await new Promise((resolve,reject)=>{denied.addEventListener('close',e=>resolve({code:e.code,reason:e.reason}),{once:true});denied.addEventListener('error',()=>{});setTimeout(()=>reject(Error('WS rejection timeout')),5000).unref()});check(closed.code===1013&&closed.reason.includes('ws_limit'),'WS race/bypass receives 1013 with explanation');report.ws_refusal=closed;
 for(const ws of websockets)ws.close();await wait(2200);
 state=await network();for(let n=0;n<4&&state.sse_clients<state.sse_limit;n++){const controller=new AbortController();streams.push(controller);const response=await fetch(base+'/api/v2/events',{signal:controller.signal});if(response.status!==200)throw Error('SSE setup '+response.status);await response.body.getReader().read();state=await network()}
 check(state.sse_clients===state.sse_limit,'SSE reaches declared limit including owner page');const extra=await fetch(base+'/api/v2/events');const sseRefusal=await extra.json();check(extra.status===503&&sseRefusal.reason==='sse_limit'&&extra.headers.get('retry-after')==='2','SSE overflow gets explained 503');
 for(const controller of streams)controller.abort();await wait(3000);
 report.before_http_reserve=await network();report.steps=[];let rejection;for(let n=0;n<21;n++){const reply=await oneHttp();report.steps.push({status:reply.status,network:await network()});if(reply.status===503){rejection=reply;break}check(reply.status===200,'bounded HTTP keep-alive accepted')}
 check(!!rejection,'soft threshold produces HTTP 503 before physical exhaustion');const body=JSON.parse(rejection.body);check(body.reason==='http_reserve'&&/retry-after:\s*2/i.test(rejection.headers)&&/connection:\s*close/i.test(rejection.headers),'reserve refusal has reason, retry and close');
 report.at_refusal=await network();check(report.at_refusal.httpd_free>0,'diagnostics retain free HTTPD slots at refusal');report.refusal=body;
 for(const s of held)s.destroy();await wait(2000);report.after=await network();check(report.after.ws_clients>=report.baseline.ws_clients,'owner WS remains connected after admission pressure');check(report.after.admission_rejected>report.baseline.admission_rejected,'rejection counters increase');
}catch(error){try{report.failure_network=await network()}catch{}report.error=String(error);console.error(error);process.exitCode=1}
finally{for(const s of held)s.destroy();for(const ws of websockets)ws.close();for(const controller of streams)controller.abort();await writeFile(new URL('report.json',output),JSON.stringify(report,null,2));}

import {project} from '@project';
import {DeviceClient} from '../client/index';
import {SnapshotPoller,decodeObservers,observerStorageKey,type ObserverPreferences} from '../client/observers';
let preferenceKey='';
const initialObservers=decodeObservers(null);
const client=new DeviceClient();
import {transmissionRate,occupiedTransmissionJobs,type Transmission} from '../contracts/transmission';
let lastTransmission:{t:number;sent:number|bigint}|null=null;
import {encodeWire,decodeWire,diagnostic,cborBody} from '../contracts/cbor';
import {reactive,computed} from 'vue';
import {t} from './appearance';
import {notify} from './notifications';
export {notify} from './notifications';
import {isEnvelope,isSummary,isInfo,isTaskPage,type Info,type Summary,type Task,type SimState,type SimEvent,type Point,type WsLog} from '../contracts';
export const ui=reactive({page:'overview',info:null as Info|null,summary:null as Summary|null,sim:null as SimState|null,tasks:[] as Task[],events:[] as SimEvent[],wsLog:[] as WsLog[],cpu:[] as Point[],coreCpu:[] as Point[][],txJobs:[] as Point[],txMemory:[] as Point[],txRate:[] as Point[],memory:[] as Point[],taskHistory:{} as Record<string,Point[]>,httpOnline:false,wsOnline:false,wsWanted:initialObservers.ws,httpWanted:initialObservers.http,refreshing:false,lastHttpSeen:0,tasksLastSeen:0,lastSource:'snapshot' as 'snapshot'|'ws'|'http',lastSeen:0,now:Date.now(),error:'',pending:false,boot:'',seq:-1,eventCursor:0,connections:[] as {id:string;state:string;rx_frames:string;tx_frames:string}[]});
let socket:WebSocket|null=null,reconnect=0,retryMs=1000,logId=0,started=false,socketGeneration=0;
export const summaryVisible=computed(()=>!!ui.summary&&((!ui.wsWanted&&!ui.httpWanted)||ui.now-ui.lastSeen<6000));
const bytes=new TextEncoder();
export const formatBytes=(n:number)=>n>=1048576?(n/1048576).toFixed(2)+t(' МиБ',' MiB'):(n/1024).toFixed(1)+t(' КиБ',' KiB');
export const clock=(ms:number)=>`${Math.floor(ms/3600000).toString().padStart(2,'0')}:${Math.floor(ms/60000)%60<10?'0':''}${Math.floor(ms/60000)%60}:${Math.floor(ms/1000)%60<10?'0':''}${Math.floor(ms/1000)%60}`;
export function log(kind:string,message:string){ui.wsLog.push({id:++logId,time:new Date().toLocaleTimeString('ru-RU'),kind,message:message.slice(0,5000)});if(ui.wsLog.length>80)ui.wsLog.shift()}
export const request=(path:string,init:RequestInit={})=>client.request(path,init);
class HttpError extends Error{constructor(readonly status:number,detail=""){super(`HTTP ${status}${detail?" · "+detail:""}`);}}
async function getCbor(path:string,signal?:AbortSignal){const r=await request(path,{signal});if(!r.ok)throw new HttpError(r.status,r.status===503?r.text:"");if(r.data===undefined)throw new Error('CBOR response required');return r.data;}
function accept(value:unknown,source:'snapshot'|'ws'|'http'){if(!isEnvelope(value)||!isSummary(value.data))throw new Error('Неверный формат summary v2');const changed=ui.boot!==value.boot_id;if(!changed&&value.seq<ui.seq)return;if(changed){ui.cpu=[];ui.coreCpu=[];ui.memory=[];ui.taskHistory={};ui.tasks=[];ui.boot=value.boot_id;ui.seq=-1;}ui.summary=value.data;ui.lastSeen=Date.now();ui.lastSource=source;if(value.seq!==ui.seq){recordTransmission(value.data.transmission,value.captured_ms,changed);ui.seq=value.seq;ui.cpu.push({t:value.captured_ms,value:value.data.cpu_busy_pct});value.data.core_busy_pct.forEach((load,index)=>{const history=ui.coreCpu[index]??=[];history.push({t:value.captured_ms,value:load});if(history.length>600)history.shift();});ui.memory.push({t:value.captured_ms,value:value.data.memory[0]?.free_bytes/1024||0});if(ui.cpu.length>600)ui.cpu.shift();if(ui.memory.length>600)ui.memory.shift();}}
function recordTransmission(stats:Transmission|undefined,capturedMs:number,reset:boolean){
 if(reset){ui.txJobs=[];ui.txMemory=[];ui.txRate=[];lastTransmission=null;}
 if(!stats)return;
 ui.txJobs.push({t:capturedMs,value:occupiedTransmissionJobs(stats)});
 ui.txMemory.push({t:capturedMs,value:stats.used_bytes});
 if(lastTransmission){const rate=transmissionRate(stats.sent_bytes,lastTransmission.sent,capturedMs-lastTransmission.t);if(rate!==null)ui.txRate.push({t:capturedMs,value:rate});}
 lastTransmission={t:capturedMs,sent:stats.sent_bytes};
 for(const history of [ui.txJobs,ui.txMemory,ui.txRate])if(history.length>600)history.shift();
}
async function tasks(signal?:AbortSignal){let offset=0;let snapshot:string|undefined;const all:Task[]=[];for(let page=0;page<8;page++){const v=await getCbor(`/api/v2/tasks?offset=${offset}&limit=8${snapshot?'&snapshot_id='+encodeURIComponent(snapshot):''}`,signal);if(!isTaskPage(v))throw new Error('Неверный формат tasks v2');if(v.boot_id!==ui.boot)return;if(snapshot&&snapshot!==v.data.snapshot_id)return;snapshot=v.data.snapshot_id;all.push(...v.data.items);if(v.data.next_offset===null){ui.tasks=all;ui.tasksLastSeen=Date.now();for(const task of all){const history=ui.taskHistory[task.id]??=[];if(history.at(-1)?.t!==v.captured_ms&&task.cpu_pct!==null)history.push({t:v.captured_ms,value:task.cpu_pct});if(history.length>120)history.shift();ui.taskHistory[task.id]=history;}return;}offset=v.data.next_offset;}}
export async function refreshSnapshot(includeInfo=true,signal?:AbortSignal,automatic=false){
 if(ui.refreshing)return;
 ui.refreshing=true;
 try{
  if(includeInfo||!ui.info){const info=await getCbor('/api/v2/info',signal);if(!isInfo(info))throw Error('Invalid info v2');ui.info=info;}
  const value=await getCbor('/api/v2/summary',signal);accept(value,automatic?'http':'snapshot');
  try{await tasks(signal);}catch(error){if(!(error instanceof HttpError&&error.status===409))throw error;}
  ui.httpOnline=true;ui.lastHttpSeen=Date.now();ui.error='';
 }catch(error){if(!signal?.aborted){ui.httpOnline=false;ui.error=error instanceof Error?error.message:'Нет связи';}}
 finally{ui.refreshing=false;}
}
const httpObserver=new SnapshotPoller(signal=>refreshSnapshot(false,signal,true));
let socketOpening=false,socketAdmission:AbortController|null=null;
function retrySocket(){if(started&&ui.wsWanted){clearTimeout(reconnect);reconnect=window.setTimeout(connect,retryMs);retryMs=Math.min(10000,retryMs*2)}}
async function connect(){
 if(!started||!ui.wsWanted||socketOpening||socket?.readyState===WebSocket.OPEN||socket?.readyState===WebSocket.CONNECTING)return;
 socketOpening=true;const generation=socketGeneration;const admissionController=socketAdmission=new AbortController();
 try{
  const admission=project.admissionPreflight?await request('/api/v2/admission?kind=ws',{signal:admissionController.signal}):null;
  if(admission&&!admission.ok){retryMs=Math.max(retryMs,Math.min(10000,Number(admission.retryAfter||0)*1000));ui.wsOnline=false;log('system',`WS admission ${admission.status}: ${admission.text}`);retrySocket();return;}
  if(!started||!ui.wsWanted||generation!==socketGeneration)return;
  const current=socket=client.openWebSocket();current.binaryType='arraybuffer';
  current.onopen=()=>{if(socket!==current||!ui.wsWanted)return;ui.wsOnline=true;retryMs=1000;log('system','OPEN /ws · CBOR v2 / binary')};
  current.onmessage=e=>{if(socket!==current||!ui.wsWanted)return;try{if(!(e.data instanceof ArrayBuffer))throw Error('Text frame rejected: CBOR binary required');const raw=new Uint8Array(e.data);const value=decodeWire(raw) as {v:number;type:string;data?:{bytes?:Uint8Array}};if(value.v!==2)throw Error('CBOR version');if(value.type==='summary'){accept(value,'ws');return;}if(value.data?.bytes instanceof Uint8Array)log('binary',`← CBOR ${raw.length} B · bytes[${value.data.bytes.length}] · `+diagnostic(value));else log(value.type==='echo.response'?'received':'system',`← CBOR ${raw.length} B · `+diagnostic(value));}catch(e){log('error','Неверное сообщение: '+String(e));}};
  current.onclose=e=>{if(socket!==current)return;socket=null;ui.wsOnline=false;log('system',`CLOSE ${e.code}${e.reason?' · '+e.reason:''}`);if(e.code===1013){retryMs=Math.max(retryMs,ui.summary?.network?.retry_after_ms??1000);notify(e.reason||t('Лимит WS: повторное подключение','WS limit: retrying'),'warning')}retrySocket()};current.onerror=()=>{};
 }catch(error){if(generation!==socketGeneration||!started||!ui.wsWanted)return;ui.wsOnline=false;log('error','WS admission: '+String(error));retrySocket()}finally{if(socketAdmission===admissionController)socketAdmission=null;socketOpening=false;if(generation!==socketGeneration&&started&&ui.wsWanted)void connect();}
}
function closeSocket(){socketGeneration++;socketAdmission?.abort();clearTimeout(reconnect);const current=socket;socket=null;ui.wsOnline=false;current?.close(1000,'user');}
function storeObservers(){try{const text=JSON.stringify({ws:ui.wsWanted,http:ui.httpWanted,sse:stream.wanted});if(localStorage.getItem(preferenceKey)!==text)localStorage.setItem(preferenceKey,text);}catch{}}
function applyObservers(value:ObserverPreferences,persist=true){
 if(ui.wsWanted!==value.ws){ui.wsWanted=value.ws;if(value.ws){if(started)void connect();}else closeSocket();}
 if(ui.httpWanted!==value.http){ui.httpWanted=value.http;if(started)httpObserver.setEnabled(value.http);}
 if(stream.wanted!==value.sse){stream.wanted=value.sse;if(value.sse){if(started)connectEvents();}else closeEvents();}
 if(persist)storeObservers();
}
export function toggleSocket(){applyObservers({ws:!ui.wsWanted,http:ui.httpWanted,sse:stream.wanted});}
export function toggleHttp(){applyObservers({ws:ui.wsWanted,http:!ui.httpWanted,sse:stream.wanted});}
export function sendEcho(text:string,binary:boolean){if(!socket||socket.readyState!==WebSocket.OPEN)return notify(t('WebSocket не подключён','WebSocket is disconnected'),'warning');const data=binary?{bytes:bytes.encode(text)}:{text};const payload=encodeWire({v:2,type:'echo.request',request_id:`r${Date.now()}`,data});if(payload.byteLength>4096)return notify(t('Кадр превышает 4096 байт','Frame exceeds 4096 bytes'),'warning');socket.send(payload);log('sent',`→ CBOR binary ${payload.length} B · `+diagnostic(data));}
export async function loadConnections(user:string,password:string){try{const r=await request('/api/v2/connections',{headers:{Authorization:'Basic '+btoa(`${user}:${password}`)}});if(!r.ok)throw new Error(`HTTP ${r.status}`);const data=r.data as {items:typeof ui.connections};ui.connections=Array.isArray(data.items)?data.items.slice(0,8):[];notify(t('Список WS-соединений обновлён','WS connections refreshed'),'success');}catch(e){notify(String(e),'error')}}
let clockTimer=0;
export function start(){
 if(started)return;
 preferenceKey=observerStorageKey(project.storageKey,location.href);
 let saved=decodeObservers(null);try{saved=decodeObservers(localStorage.getItem(preferenceKey));}catch{}
 applyObservers(saved,false);started=true;
 void refreshSnapshot().finally(()=>{if(!started)return;void connect();httpObserver.setEnabled(ui.httpWanted,false);connectEvents();});
 if(!clockTimer)clockTimer=window.setInterval(()=>ui.now=Date.now(),1000);
}

export interface Diagnostic {v:2;source:'device'|'simulator';boot_id:string;seq:number;captured_ms:number;cpu_busy_pct:number;free_bytes:number;task_count:number}
export const stream=reactive({online:false,wanted:initialObservers.sse,count:0,lastSeen:0,id:'',error:'',latest:null as Diagnostic|null,points:[] as Point[],log:[] as {id:string;data:Diagnostic}[]});
let eventSource:EventSource|null=null,eventReconnect=0,eventRetryMs=1000;
function connectEvents(){
    if(!started||!stream.wanted||!ui.info?.capabilities.sse?.supported||eventSource)return;
    clearTimeout(eventReconnect);
    const source=eventSource=client.openEvents();
    eventSource.onopen=()=>{if(eventSource!==source||!stream.wanted)return;stream.online=true;stream.error='';eventRetryMs=1000;};
    source.onerror=()=>{
        if(eventSource!==source)return;
        stream.online=false;stream.error='SSE reconnect';source.close();eventSource=null;
        // A temporary 503 is terminal for native EventSource. Retry explicitly
        // after the previous page's async SSE session has had time to close.
        if(started&&stream.wanted){eventRetryMs=Math.max(eventRetryMs,ui.summary?.network?.retry_after_ms??1000);eventReconnect=window.setTimeout(connectEvents,eventRetryMs);eventRetryMs=Math.min(10000,eventRetryMs*2);}
    };
    eventSource.addEventListener('diagnostic',event=>{
        try{
            const message=event as MessageEvent<string>;const d=JSON.parse(message.data) as Diagnostic;
            if(d.v!==2||!['device','simulator'].includes(d.source)||typeof d.boot_id!=='string'||!Number.isSafeInteger(d.seq)||![d.captured_ms,d.cpu_busy_pct,d.free_bytes,d.task_count].every(Number.isFinite))throw Error('Invalid diagnostic');
            if(stream.latest?.boot_id!==d.boot_id){stream.points=[];stream.log=[];}
            stream.online=true;stream.error='';stream.lastSeen=Date.now();stream.id=message.lastEventId;stream.count++;stream.latest=d;
            if(stream.points.at(-1)?.t!==d.captured_ms){stream.points.push({t:d.captured_ms,value:d.free_bytes/1024});if(stream.points.length>120)stream.points.shift();}
            stream.log.unshift({id:`${stream.count}:${message.lastEventId}`,data:d});stream.log=stream.log.slice(0,80);
        }catch(error){stream.error=String(error);}
    });
}
function closeEvents(){clearTimeout(eventReconnect);eventSource?.close();eventSource=null;stream.online=false;}
export function toggleEvents(){applyObservers({ws:ui.wsWanted,http:ui.httpWanted,sse:!stream.wanted});}
window.addEventListener('storage',event=>{if(event.key===preferenceKey)applyObservers(decodeObservers(event.newValue),false);});
window.addEventListener('pagehide',()=>{started=false;httpObserver.setEnabled(false);closeEvents();closeSocket();});
window.addEventListener('pageshow',event=>{if(event.persisted)start();});
import {watch} from 'vue';
watch(()=>ui.info?.capabilities.sse?.supported,supported=>{if(supported)connectEvents();else closeEvents();});

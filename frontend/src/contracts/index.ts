import {isTransmission,type Transmission} from './transmission.ts';
export {isTransmission,transmissionRate,type Transmission} from './transmission.ts';
import {isMemoryUsageRegion,type MemoryUsageRegion} from './memory.ts';
export interface Envelope<T>{v:2;source:'device'|'simulator';boot_id:string;seq:number;captured_ms:number;data:T}
export interface Capability{supported:boolean;origin:string;device_status:string;reason?:string|null}
export interface Info{v:2;source:'device'|'simulator';boot_id:string;chip:{model:string;revision_major:number;revision_minor:number;cores:number;psram:boolean};firmware:{name:string;version:string;idf_version:string;http_backend?:'tcp'|'idf_httpd';json_backend?:string;mcp_enabled?:boolean;json_workspace_bytes?:number;mcp_output_bytes?:number;mcp_input_bytes?:number;board_label:string};details?:Record<string,Record<string,string|number|boolean|null>>;capabilities:Record<string,Capability>;limits:{frame_bytes:number;body_bytes:number;task_page_max:number;max_ws_clients:number}}
export interface Pool{id:string;label:string;total_bytes:number;free_bytes:number;min_free_bytes:number;largest_block_bytes:number}
export interface Counters{http_requests:number|bigint;ws_rx_frames:number|bigint;ws_tx_frames:number|bigint;ws_rx_bytes:number|bigint;ws_tx_bytes:number|bigint;rejected:number|bigint}
export interface NetworkState{tcp_pcb_limit:number;socket_limit:number;reserved_sockets:number;httpd_limit:number;httpd_open:number;httpd_free:number;http_reserve:number;ws_limit:number;ws_clients:number;sse_limit:number;sse_clients:number;admission_rejected:number;last_rejection:string;retry_after_ms:number;captured_ms:number;ready:boolean}
export interface Summary{transmission?:Transmission;network?:NetworkState;uptime_ms:number;sample_interval_ms:number;cpu_busy_pct:number;core_busy_pct:number[];memory:Pool[];memory_usage?:MemoryUsageRegion[];rssi_dbm:number|null;task_count:number;ws_clients:number;network_status:string;counters:Counters;experiment:Experiment;led:Led;strip:Strip}
export interface Task{id:string;name:string;state:string;priority:number;core_affinity:number|null;stack_min_free_bytes:number;runtime_ticks:number|bigint;cpu_pct:number|null}
export interface TaskPage{snapshot_id:string;offset:number;next_offset:number|null;total:number;items:Task[]}
export interface SimState{profile:string;scenario:string;paused:boolean;speed:number;seed:number;time_ms:number;boot_id:string;network_offline:boolean;sequence:number}
export interface SimEvent{id:number;captured_ms:number;level:string;category:string;message:string}
export interface Point{t:number;value:number}
export interface WsLog{id:number;time:string;kind:string;message:string}
const object=(v:unknown):v is Record<string,unknown>=>!!v&&typeof v==='object'&&!Array.isArray(v);
const finite=(v:unknown)=>typeof v==='number'&&Number.isFinite(v);
const optionalByteCount=(v:unknown)=>v===undefined||(typeof v==='number'&&Number.isFinite(v)&&Number.isInteger(v)&&v>=0);
export function isEnvelope(v:unknown):v is Envelope<unknown>{return object(v)&&v.v===2&&(v.source==='simulator'||v.source==='device')&&typeof v.boot_id==='string'&&finite(v.seq)&&finite(v.captured_ms)&&object(v.data)}
export function isSummary(v:unknown):v is Summary{if(!object(v))return false;return (v.transmission===undefined||isTransmission(v.transmission))&&(v.memory_usage===undefined||(Array.isArray(v.memory_usage)&&v.memory_usage.length<=4&&v.memory_usage.every(isMemoryUsageRegion)))&&isStrip(v.strip)&&isExperiment(v.experiment)&&isLed(v.led)&&finite(v.uptime_ms)&&finite(v.cpu_busy_pct)&&finite(v.task_count)&&finite(v.ws_clients)&&Array.isArray(v.core_busy_pct)&&v.core_busy_pct.length<=4&&v.core_busy_pct.every(finite)&&Array.isArray(v.memory)&&v.memory.length<=4&&v.memory.every(p=>object(p)&&typeof p.id==='string'&&finite(p.total_bytes)&&finite(p.free_bytes)&&finite(p.min_free_bytes)&&finite(p.largest_block_bytes))&&object(v.counters)&&['http_requests','ws_rx_frames','ws_tx_frames','ws_rx_bytes','ws_tx_bytes','rejected'].every(k=>typeof v.counters==='object'&&v.counters!==null&&isCounter((v.counters as Record<string,unknown>)[k]))}
export function isInfo(v:unknown):v is Info{return object(v)&&v.v===2&&(v.source==='simulator'||v.source==='device')&&object(v.chip)&&typeof v.chip.model==='string'&&finite(v.chip.cores)&&object(v.firmware)&&typeof v.firmware.version==='string'&&(v.firmware.http_backend===undefined||v.firmware.http_backend==='tcp'||v.firmware.http_backend==='idf_httpd')&&(v.firmware.json_backend===undefined||typeof v.firmware.json_backend==='string')&&(v.firmware.mcp_enabled===undefined||typeof v.firmware.mcp_enabled==='boolean')&&optionalByteCount(v.firmware.json_workspace_bytes)&&optionalByteCount(v.firmware.mcp_output_bytes)&&optionalByteCount(v.firmware.mcp_input_bytes)&&object(v.capabilities)&&object(v.limits)&&finite(v.limits.frame_bytes)}

export function httpBackendName(backend:Info['firmware']['http_backend']):string{return backend==='tcp'?'NaWServer TCP':backend==='idf_httpd'?'ESP-IDF HTTPD':'—'}
export function isTaskPage(v:unknown):v is Envelope<TaskPage>{return isEnvelope(v)&&object(v.data)&&Array.isArray(v.data.items)&&v.data.items.length<=8&&v.data.items.every(i=>object(i)&&typeof i.id==='string'&&typeof i.name==='string'&&finite(i.stack_min_free_bytes)&&isCounter(i.runtime_ticks)&&(i.cpu_pct===null||finite(i.cpu_pct)))&&typeof v.data.snapshot_id==='string'&&finite(v.data.total)}

function isCounter(v:unknown){return typeof v==='bigint'?v>=0n:typeof v==='number'&&Number.isSafeInteger(v)&&v>=0}


export interface Experiment{running:boolean;kind:string;duration_ms:number;on_ms:number;period_ms:number;core:number;priority:number;elapsed_ms:number;progress_pct:number;pi:number;iterations:number|bigint;active:boolean;sample_ms:number}
export interface Led{supported:boolean;enabled:boolean;period_ms:number;duty_pct:number;gpio:number;active_low:boolean;level:boolean}

function isExperiment(v:unknown):v is Experiment{return object(v)&&typeof v.running==='boolean'&&(v.kind==='pi'||v.kind==='pulse')&&['duration_ms','on_ms','period_ms','core','priority','elapsed_ms','progress_pct','pi','sample_ms'].every(k=>finite(v[k]))&&isCounter(v.iterations)&&typeof v.active==='boolean'}
export function isLed(v:unknown):v is Led{return object(v)&&typeof v.supported==='boolean'&&typeof v.enabled==='boolean'&&typeof v.active_low==='boolean'&&typeof v.level==='boolean'&&finite(v.period_ms)&&finite(v.duty_pct)&&finite(v.gpio)}

export interface Strip{available_drivers?:('rmt'|'spi')[];driver:'rmt'|'spi';requested_driver:'rmt'|'spi';driver_error:string;supported:boolean;enabled:boolean;gpio:number;count:number;mode:string;color:number;brightness_pct:number;period_ms:number;pixels:number[]}


export function isStrip(v:unknown):v is Strip{return object(v)&&(v.driver==='rmt'||v.driver==='spi')&&(v.requested_driver==='rmt'||v.requested_driver==='spi')&&typeof v.driver_error==='string'&&typeof v.supported==='boolean'&&typeof v.enabled==='boolean'&&typeof v.mode==='string'&&['gpio','count','color','brightness_pct','period_ms'].every(k=>finite(v[k]))&&Array.isArray(v.pixels)&&typeof v.count==='number'&&Number.isInteger(v.count)&&v.count>=1&&v.count<=10&&v.pixels.length===v.count&&v.pixels.every(n=>typeof n==='number'&&Number.isInteger(n)&&n>=0&&n<=0xffffff)}




export function jsonBackendName(backend:Info['firmware']['json_backend']):string{return backend==='nawserver_bounded'?'NaWServer bounded JSON':backend==='yyjson'?'yyjson':backend==='system_text_json'?'System.Text.Json':backend==='none'?'— (MCP off)':backend||'—'}




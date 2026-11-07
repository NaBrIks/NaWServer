// Browser test fixture only. Never linked into firmware or presented as hardware evidence.
import http from 'node:http';import {readFile} from 'node:fs/promises';import {createHash} from 'node:crypto';
import {encode,decode} from 'cborg';
const pack=v=>v instanceof Uint8Array||v===null||typeof v!=='object'?v:Array.isArray(v)?v.map(pack):new Map(Object.entries(v).map(([k,x])=>{return[k,pack(x)]}));
const unpack=v=>v instanceof Map?Object.fromEntries([...v].map(([k,x])=>[k,unpack(x)])):Array.isArray(v)?v.map(unpack):v;
const wire=v=>Buffer.from(encode(pack(v))),parse=v=>unpack(decode(v,{useMaps:true}));
const boot='TEST-S3-DO-NOT-FLASH',start=Date.now();let seq=0;
const display={ready:true,width:800,height:480,pattern:0,backlight:true,pressed:false,x:0,y:0,touches:23,touch_errors:0,renderer:'lvgl-8.3.11',framebuffers:2,frame_swaps:144,frame_events:12000,vsyncs:12000};
const experiment={running:false,kind:'pi',duration_ms:5000,on_ms:200,period_ms:1000,core:0,priority:2,elapsed_ms:0,progress_pct:0,pi:0,iterations:0,active:false,sample_ms:100};
const led={supported:false,enabled:false,period_ms:1000,duty_pct:50,gpio:-1,active_low:true,level:false};
const strip={driver:'rmt',requested_driver:'rmt',driver_error:'',supported:true,enabled:false,gpio:6,count:10,mode:'solid',color:0xffa030,brightness_pct:20,period_ms:1000,pixels:Array(10).fill(0)};
const counters={http_requests:12345,ws_rx_frames:4,ws_tx_frames:123,ws_rx_bytes:256,ws_tx_bytes:65536,rejected:0};
const capability=supported=>({supported,origin:'simulator',device_status:supported?'test_fixture':'unsupported'});
const info={v:2,source:'simulator',boot_id:boot,chip:{model:'ESP32-S3',revision_major:0,revision_minor:2,cores:2,psram:true},firmware:{name:'TEST FIXTURE',version:'UI verification only',idf_version:'5.4.0',board_label:'TEST DATA · Waveshare S3 Touch LCD 7'},capabilities:Object.fromEntries(['tasks','memory','experiments','led','strip','connections','simulator_control','sse','authenticated_ws'].map(k=>[k,capability(!['led','connections','simulator_control','authenticated_ws'].includes(k))])),limits:{frame_bytes:4096,body_bytes:4096,task_page_max:8,max_ws_clients:4}};
const network=()=>({socket_limit:24,tcp_pcb_limit:32,reserved_sockets:3,httpd_limit:21,httpd_open:8,httpd_free:13,http_reserve:3,ws_limit:4,ws_clients:sockets.size,sse_limit:4,sse_clients:events.size,admission_rejected:0,last_rejection:'none',retry_after_ms:2000,captured_ms:Date.now()-start,ready:true});
const summary=()=>({uptime_ms:Date.now()-start,sample_interval_ms:1000,cpu_busy_pct:12.3,core_busy_pct:[8.2,16.4],memory:[{id:'internal',label:'Внутренняя RAM',total_bytes:327680,free_bytes:174321,min_free_bytes:160000,largest_block_bytes:123456},{id:'psram',label:'PSRAM',total_bytes:8388608,free_bytes:5735208,min_free_bytes:5700000,largest_block_bytes:4000000}],rssi_dbm:-48,task_count:4,ws_clients:sockets.size,network_status:'connected',counters,experiment,led,strip,display,network:network()});
const envelope=data=>({v:2,source:'simulator',boot_id:boot,seq:++seq,captured_ms:Date.now()-start,data});
const tasks=['IDLE0','IDLE1','lcd7','httpd'].map((name,i)=>({id:'task-'+i,name,state:'blocked',priority:i,core_affinity:i%2,stack_min_free_bytes:1024+i*123,runtime_ticks:100000+i*12345,cpu_pct:i===2?12.3:0}));
const sockets=new Set();const events=new Set();
const server=http.createServer(async(req,res)=>{try{
 const url=new URL(req.url,'http://127.0.0.1:8790');const path=url.pathname;let body=Buffer.alloc(0);for await(const b of req)body=Buffer.concat([body,b]);
 const text=(code,value)=>{res.writeHead(code,{'Content-Type':'text/plain'});res.end(value)};const cbor=value=>{res.writeHead(200,{'Content-Type':'application/cbor'});res.end(wire(value))};
 if(path==='/api/v2/network')return cbor(network());
 if(path==='/api/v2/admission')return cbor({ok:true,retry_after_ms:2000,network:network()});
 if(path.startsWith('/api/v2/auth')||path==='/api/v2/events/private'||req.method==='POST'&&path.startsWith('/api/v2/')&&path!=='/api/v2/echo'){
  if(req.headers.authorization!=='Basic '+Buffer.from('demo:showcase-only').toString('base64')){res.writeHead(401,{'WWW-Authenticate':'Basic realm="test"'});res.end('Unauthorized');return}
  if(path.endsWith('/admin'))return text(403,'Forbidden');
 }
 if(path.startsWith('/api/v2/events')){res.writeHead(200,{'Content-Type':'text/event-stream','Cache-Control':'no-store'});res.write(': test fixture\n\n');events.add(res);req.on('close',()=>events.delete(res));return}
 if(path==='/api/v2/info')return cbor(info);
 if(path==='/api/v2/summary')return cbor(envelope(summary()));
 if(path==='/api/v2/tasks')return cbor(envelope({snapshot_id:'test-page',offset:0,next_offset:null,total:tasks.length,items:tasks}));
 if(path==='/api/status'){res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(display));return}
 if(path==='/api/display'){
  if(req.headers['x-lcd7-command']!=='1')return text(403,'Command header required');const command=body.toString();const index=['bars','grid','red','green','blue'].indexOf(command);if(index>=0)display.pattern=index;else if(command==='on'||command==='off')display.backlight=command==='on';else return text(400,'Unknown command');display.frame_swaps++;return text(202,'Queued');
 }
 if(path==='/api/v2/network')return cbor(network());
 if(path==='/api/v2/admission')return cbor({ok:true,retry_after_ms:2000,network:network()});
 if(path.startsWith('/api/v2/auth'))return text(200,'Authenticated: demo');
 if(path==='/api/v2/experiments'){
  if(req.method==='POST'){const c=parse(body);experiment.running=c.action!=='stop';if(experiment.running){Object.assign(experiment,{...c,running:true});setTimeout(()=>{experiment.running=false;experiment.progress_pct=100},Math.min(c.duration_ms??5000,5000))}}return cbor(experiment);
 }
 if(path==='/api/v2/strip'){if(req.method==='POST')Object.assign(strip,parse(body));return cbor(strip)}
 if(path==='/api/v2/led')return cbor(led);
 if(path==='/api/v2/echo')return text(200,req.method==='POST'?new URLSearchParams(body.toString()).get('value')??'':url.searchParams.get('value')??'');
 if(path==='/api/v2/stream')return text(200,'headers → buffered content → response. Not SSE.');
 if(path==='/go'){res.writeHead(302,{Location:'/#experiments'});res.end();return}
 if(path.startsWith('/api/'))return text(501,'Not implemented in test fixture');
 const relative=path==='/'?'index.html':path.slice(1);if(relative.includes('..'))return text(400,'Invalid path');
 const file=await readFile(new URL('../dist/'+relative,import.meta.url));const ext=relative.split('.').at(-1);res.writeHead(200,{'Content-Type':({html:'text/html',js:'text/javascript',css:'text/css',woff:'font/woff',txt:'text/plain'})[ext]??'application/octet-stream'});res.end(file);
 }catch(error){res.writeHead(500);res.end(String(error))}});
function frame(socket,value){const bytes=wire(value);let header;if(bytes.length<126)header=Buffer.from([0x82,bytes.length]);else{header=Buffer.alloc(4);header[0]=0x82;header[1]=126;header.writeUInt16BE(bytes.length,2)}socket.write(Buffer.concat([header,bytes]))}
server.on('upgrade',(req,socket)=>{if(req.url!=='/ws'){socket.destroy();return}const accept=createHash('sha1').update(req.headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n');sockets.add(socket);let pending=Buffer.alloc(0);socket.on('data',bytes=>{try{pending=Buffer.concat([pending,bytes]);while(pending.length>=6){let size=pending[1]&127,offset=2;if(size===126){if(pending.length<8)return;size=pending.readUInt16BE(2);offset=4}if(size===127||size>4096){socket.destroy();return}if(pending.length<offset+4+size)return;const opcode=pending[0]&15;const mask=pending.subarray(offset,offset+4),data=Buffer.from(pending.subarray(offset+4,offset+4+size));pending=pending.subarray(offset+4+size);for(let i=0;i<data.length;i++)data[i]^=mask[i%4];if(opcode===8){socket.end();return}const msg=parse(data);frame(socket,{v:2,type:'echo.response',request_id:msg.request_id,data:msg.data})}}catch{socket.destroy()}});socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>sockets.delete(socket))});
setInterval(()=>{const value={...envelope(summary()),type:'summary'};for(const socket of sockets)frame(socket,value);const diagnostic={v:2,source:'simulator',boot_id:boot,seq:++seq,captured_ms:Date.now()-start,cpu_busy_pct:12.3,free_bytes:174321,task_count:4};for(const response of events)response.write(`event: diagnostic\nid: ${seq}\ndata: ${JSON.stringify(diagnostic)}\n\n`)},1000).unref();
server.listen(8790,'127.0.0.1',()=>console.log('TEST FIXTURE only: http://127.0.0.1:8790'));

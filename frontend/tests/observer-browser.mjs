import {createServer as httpServer} from 'node:http';
import {createHash} from 'node:crypto';
import {createServer as viteServer} from 'vite';
import {chromium} from 'playwright';
import {encode} from 'cborg';
import {mkdirSync,writeFileSync,existsSync,readdirSync} from 'node:fs';
import {resolve} from 'node:path';
import assert from 'node:assert/strict';
const output=resolve('local/observer-browser');mkdirSync(output,{recursive:true});
const counters={info:0,summary:0,tasks:0,wsOpened:0,sseOpened:0,wsSent:0,sseSent:0};
const sockets=new Set(),events=new Set();let sequence=0;
const info={v:2,source:'device',boot_id:'observer-fixture',chip:{model:'RP2350',revision_major:2,revision_minor:0,cores:2,psram:false},firmware:{name:'Fixture',version:'test',idf_version:'test',board_label:'Controlled fixture'},capabilities:{sse:{supported:true},tasks:{supported:true}},limits:{frame_bytes:4096,body_bytes:4096,task_page_max:8,max_ws_clients:4}};
const base={uptime_ms:10000,sample_interval_ms:100,cpu_busy_pct:37.5,core_busy_pct:[25,50],memory:[{id:'internal',label:'Fixture heap',total_bytes:196608,free_bytes:131072,min_free_bytes:130000,largest_block_bytes:120000}],rssi_dbm:-60,task_count:9,ws_clients:1,network_status:'connected',counters:{http_requests:1,ws_rx_frames:1,ws_tx_frames:1,ws_rx_bytes:1,ws_tx_bytes:1,rejected:0},experiment:{running:false,kind:'pi',duration_ms:1,on_ms:1,period_ms:1,core:0,priority:1,elapsed_ms:0,progress_pct:0,pi:0,sample_ms:100,iterations:0,active:false},led:{supported:false,enabled:false,active_low:false,level:false,period_ms:1000,duty_pct:0,gpio:0},strip:{driver:'rmt',requested_driver:'rmt',driver_error:'',supported:false,enabled:false,gpio:0,count:1,mode:'solid',color:0,brightness_pct:0,period_ms:1000,pixels:[0]}};
const tasks=Array.from({length:9},(_,i)=>({id:String(i),name:i<2?'IDLE'+i:'Task'+i,state:'ready',priority:1,core_affinity:0,stack_min_free_bytes:2048,runtime_ticks:1,cpu_pct:10}));
const envelope=data=>({v:2,source:'device',boot_id:'observer-fixture',seq:++sequence,captured_ms:sequence*100,data});
const api=httpServer((request,response)=>{
 const url=new URL(request.url,'http://fixture');
 if(url.pathname==='/api/v2/events'){
  counters.sseOpened++;events.add(response);response.writeHead(200,{'Content-Type':'text/event-stream','Cache-Control':'no-cache'});response.write(': open\n\n');
  const timer=setInterval(()=>{counters.sseSent++;response.write(`event: diagnostic\nid: ${sequence}\ndata: ${JSON.stringify({...envelope(undefined),cpu_busy_pct:99,free_bytes:131072,task_count:9})}\n\n`);},100);
  request.on('close',()=>{clearInterval(timer);events.delete(response);});return;
 }
 let result;
 if(url.pathname==='/api/v2/info'){counters.info++;result=info;}
 else if(url.pathname==='/api/v2/summary'){counters.summary++;result=envelope(base);}
 else if(url.pathname==='/api/v2/tasks'){counters.tasks++;const offset=Number(url.searchParams.get('offset')??0);result=envelope({snapshot_id:'fixture',offset,next_offset:offset+8<tasks.length?offset+8:null,total:tasks.length,items:tasks.slice(offset,offset+8)});}
 else{response.writeHead(404);response.end();return;}
 response.writeHead(200,{'Content-Type':'application/cbor'});response.end(Buffer.from(encode(result)));
});
function binaryFrame(payload){const head=payload.length<126?Buffer.from([0x82,payload.length]):Buffer.from([0x82,126,payload.length>>8,payload.length&255]);return Buffer.concat([head,payload]);}
api.on('upgrade',(request,socket)=>{
 if(new URL(request.url,'http://fixture').pathname!=='/ws'){socket.destroy();return;}
 const accept=createHash('sha1').update(request.headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
 socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n');
 counters.wsOpened++;sockets.add(socket);
 const timer=setInterval(()=>{counters.wsSent++;socket.write(binaryFrame(Buffer.from(encode({...envelope(base),type:'summary'}))));},100);
 socket.on('data',bytes=>{if((bytes[0]&15)===8){socket.write(Buffer.from([0x88,0]));socket.end();}});
 socket.on('close',()=>{clearInterval(timer);sockets.delete(socket);});socket.on('error',()=>{});
});
await new Promise(resolve=>api.listen(0,'127.0.0.1',resolve));
process.env.NAWSERVER_DEVICE_URL='http://127.0.0.1:'+api.address().port;
const vite=await viteServer({configFile:resolve('frontend/vite.config.ts'),mode:'rp2350_pico2_w',server:{host:'127.0.0.1',port:0}});await vite.listen();
const origin=vite.resolvedUrls.local[0];
const browserRoot=resolve(process.env.LOCALAPPDATA??'','ms-playwright');
const executable=process.env.NAWSERVER_BROWSER_EXECUTABLE??readdirSync(browserRoot).filter(x=>x.startsWith('chromium_headless_shell-')).map(x=>resolve(browserRoot,x,'chrome-win/headless_shell.exe')).find(existsSync);
const browser=await chromium.launch({executablePath:executable,headless:true});
const context=await browser.newContext({viewport:{width:1360,height:1000}});const page=await context.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(predicate){const end=Date.now()+5000;while(!await predicate()){assert(Date.now()<end,'browser state timeout');await pause(20);}}
const httpCounts=()=>({info:counters.info,summary:counters.summary,tasks:counters.tasks});
const geometry=()=>page.evaluate(()=>({header:document.querySelector('.topbar').getBoundingClientRect().toJSON(),metrics:document.querySelector('.metrics').getBoundingClientRect().toJSON(),panels:[...document.querySelectorAll('.resource-charts>.panel')].map(x=>x.getBoundingClientRect().toJSON()),scroll:document.querySelector('.content-column').scrollTop}));
try{
 await page.goto(origin);await until(()=>sockets.size===1&&events.size===1&&counters.tasks===2);
 assert.equal(await page.getByTestId('http-toggle').getAttribute('aria-pressed'),'false');
 const idle=httpCounts();await pause(1400);assert.deepEqual(httpCounts(),idle,'hidden default HTTP polling');
 const before=await geometry();const sseBefore=counters.sseSent;
 await page.getByTestId('ws-toggle').click();await until(()=>sockets.size===0);await pause(250);assert(counters.sseSent>sseBefore);assert.equal(events.size,1);
 await page.getByTestId('sse-toggle').click();await until(()=>events.size===0);await pause(1200);assert.equal(sockets.size,0);assert.equal(events.size,0);assert.deepEqual(httpCounts(),idle);
 assert.equal(await page.getByTestId('info-exchange').innerText(),'Снимок');assert.deepEqual(await geometry(),before,'toggle geometry changed');
 await page.getByTestId('http-refresh').click();await until(()=>counters.info===2&&counters.summary===2&&counters.tasks===4);const manual=httpCounts();await pause(1200);assert.deepEqual(httpCounts(),manual);
 await page.getByTestId('http-toggle').click();await until(()=>counters.summary>=manual.summary+2);assert.equal(counters.info,2,'info is repeatedly polled');
 await page.getByTestId('http-toggle').click();await until(async()=>!await page.getByTestId('http-refresh').isDisabled());await pause(100);const stopped=httpCounts();await pause(1300);assert.deepEqual(httpCounts(),stopped,'HTTP continued after disable');
 assert.equal(sockets.size,0);assert.equal(events.size,0);
 await page.getByTestId('ws-toggle').click();await until(()=>sockets.size===1);const opened=counters.wsOpened;for(const socket of sockets)socket.destroy();await until(()=>counters.wsOpened>opened&&sockets.size===1);
 await page.reload();await until(()=>sockets.size===1&&counters.info===3);assert.equal(events.size,0);assert.equal(await page.getByTestId('sse-toggle').getAttribute('aria-pressed'),'false');
 const other=await context.newPage();await other.goto(origin);await until(()=>sockets.size===2);await other.getByTestId('ws-toggle').click();await until(()=>sockets.size===0);assert.equal(await page.getByTestId('ws-toggle').getAttribute('aria-pressed'),'false','cross-tab preference did not apply');await other.close();
 await pause(1200);assert.equal(sockets.size,0);assert.equal(events.size,0);
 const frozen=await page.getByTestId('cpu-value').innerText();await page.getByTestId('sse-toggle').click();await until(()=>events.size===1);await pause(300);assert.equal(sockets.size,0);assert.equal(await page.getByTestId('cpu-value').innerText(),frozen,'SSE silently changed full INFO');await page.getByTestId('sse-toggle').click();await until(()=>events.size===0);
 assert.deepEqual(errors,[]);await page.mouse.move(600,500);await page.waitForTimeout(150);await page.screenshot({path:resolve(output,'observers-off.png'),fullPage:false});
 writeFileSync(resolve(output,'result.json'),JSON.stringify({passed:true,counters,defaultHttp:idle,afterManual:manual,afterStopped:stopped,geometryStable:true,errors},null,2));
 console.log('OBSERVER_BROWSER_PASS: independent native WS/SSE, explicit HTTP1Hz, no hidden polling, persistence/reconnect/cross-tab, stable geometry');
}catch(error){console.error('OBSERVER_DEBUG',JSON.stringify({origin,counters,ws:sockets.size,sse:events.size,errors,status:await page.getByTestId('info-exchange').innerText().catch(()=>'<not rendered>')}));await page.screenshot({path:resolve(output,'failure.png')});throw error;}finally{await browser.close();for(const response of events)response.end();for(const socket of sockets)socket.destroy();api.closeAllConnections();await new Promise(resolve=>api.close(resolve));await vite.close();}

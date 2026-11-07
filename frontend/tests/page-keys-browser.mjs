import {createServer as httpServer} from 'node:http';
import {createServer as viteServer} from 'vite';
import {chromium} from 'playwright';
import {encode,decode} from 'cborg';
import {resolve} from 'node:path';
import {mkdirSync,readdirSync,writeFileSync,existsSync} from 'node:fs';
import assert from 'node:assert/strict';
let sequence=0;
const info={v:2,source:'device',boot_id:'observer-fixture',chip:{model:'RP2350',revision_major:2,revision_minor:0,cores:2,psram:false},firmware:{name:'Fixture',version:'test',idf_version:'test',board_label:'Controlled fixture'},capabilities:{sse:{supported:true},tasks:{supported:true}},limits:{frame_bytes:4096,body_bytes:4096,task_page_max:8,max_ws_clients:4}};
const base={uptime_ms:10000,sample_interval_ms:100,cpu_busy_pct:37.5,core_busy_pct:[25,50],memory:[{id:'internal',label:'Fixture heap',total_bytes:196608,free_bytes:131072,min_free_bytes:130000,largest_block_bytes:120000}],rssi_dbm:-60,task_count:9,ws_clients:1,network_status:'connected',counters:{http_requests:1,ws_rx_frames:1,ws_tx_frames:1,ws_rx_bytes:1,ws_tx_bytes:1,rejected:0},experiment:{running:false,kind:'pi',duration_ms:1,on_ms:1,period_ms:1,core:0,priority:1,elapsed_ms:0,progress_pct:0,pi:0,sample_ms:100,iterations:0,active:false},led:{supported:false,enabled:false,active_low:false,level:false,period_ms:1000,duty_pct:0,gpio:0},strip:{driver:'rmt',requested_driver:'rmt',driver_error:'',supported:false,enabled:false,gpio:0,count:1,mode:'solid',color:0,brightness_pct:0,period_ms:1000,pixels:[0]}};
const tasks=Array.from({length:9},(_,i)=>({id:String(i),name:i<2?'IDLE'+i:'Task'+i,state:'ready',priority:1,core_affinity:0,stack_min_free_bytes:2048,runtime_ticks:1,cpu_pct:10}));
const envelope=data=>({v:2,source:'device',boot_id:'observer-fixture',seq:++sequence,captured_ms:sequence*100,data});
const slots=Array.from({length:4},(_,slot)=>({slot,name:slot===0?'Collector with a long descriptive label':'',active:slot===0,revision:slot===0?12:0}));
const labels=[],secrets=[],errors=[];let deny=false;
const api=httpServer(async(request,response)=>{
 const url=new URL(request.url,'http://fixture');labels.push(request.headers['x-page-id']);let data;
 if(url.pathname==='/api/v2/keys'){
  if(deny){response.writeHead(401);response.end();return;}
  if(request.method==='POST'){let bytes=[];for await(const chunk of request)bytes.push(chunk);const command=decode(Buffer.concat(bytes));const row=slots[command.slot];row.active=command.action!=='revoke';row.name=command.name;row.revision++;data={slot:row.slot,...(row.active?{secret:'fixture-secret-'+row.revision}:{})};secrets.push(data.secret);}
  else data={maximum_slots:4,slots};
 }else if(url.pathname==='/api/v2/info')data=info;
 else if(url.pathname==='/api/v2/summary')data=envelope(base);
 else if(url.pathname==='/api/v2/tasks')data=envelope({snapshot_id:'fixture',offset:0,next_offset:null,total:0,items:[]});
 else {response.writeHead(404);response.end();return;}
 response.writeHead(200,{'Content-Type':'application/cbor'});response.end(Buffer.from(encode(data)));
});
api.on('upgrade',(_request,socket)=>socket.destroy());await new Promise(r=>api.listen(0,'127.0.0.1',r));
process.env.NAWSERVER_DEVICE_URL='http://127.0.0.1:'+api.address().port;
const vite=await viteServer({configFile:resolve('frontend/vite.config.ts'),mode:'rp2350_pico2_w',server:{host:'127.0.0.1',port:0}});await vite.listen();
const root=resolve(process.env.LOCALAPPDATA??'','ms-playwright');const executable=process.env.NAWSERVER_BROWSER_EXECUTABLE??readdirSync(root).filter(x=>x.startsWith('chromium_headless_shell-')).map(x=>resolve(root,x,'chrome-win/headless_shell.exe')).find(existsSync);
const browser=await chromium.launch({executablePath:executable,headless:true});const context=await browser.newContext({viewport:{width:1440,height:1100}}),page=await context.newPage();page.on('pageerror',error=>errors.push(String(error)));
const output=resolve('local/page-keys-browser');mkdirSync(output,{recursive:true});
try{
 await page.goto(vite.resolvedUrls.local[0]+'#access');await page.getByTestId('keys-load').click();await page.getByText('Активен',{exact:true}).waitFor();
 const table=page.getByTestId('keys-table');const geometry=()=>table.evaluate(element=>{const r=element.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height,scroll:document.querySelector('.content-column')?.scrollTop};});
 const before=await geometry();deny=true;await page.getByTestId('keys-load').click();await page.waitForTimeout(150);assert.deepEqual(await geometry(),before,'error changed key table geometry');
 deny=false;await page.getByTestId('keys-load').click();await page.waitForTimeout(150);assert.deepEqual(await geometry(),before,'success changed key table geometry');
 await page.getByRole('button',{name:'Заменить',exact:true}).click();await page.getByTestId('key-secret').waitFor();assert.equal(await page.getByTestId('key-secret').inputValue(),'fixture-secret-13');
 await page.locator('dialog.api-key-secret form button').last().click();await page.waitForFunction(()=>document.querySelector('[data-testid=key-secret]').value==='');assert.equal(await page.getByTestId('key-secret').inputValue(),'');assert.deepEqual(await geometry(),before,'rotation changed key table geometry');
 await page.getByRole('button',{name:'Отозвать',exact:true}).click();await page.waitForTimeout(200);assert.equal(await page.getByText('Активен',{exact:true}).count(),0);assert.deepEqual(await geometry(),before,'revoke changed key table geometry');
 await page.screenshot({path:resolve(output,'access-cream-ru.png')});
 const first=labels.find(Boolean);assert.match(first,/^[a-f0-9]{32}$/);await page.reload();await page.waitForTimeout(300);const second=labels.filter(Boolean).at(-1);assert.notEqual(second,first,'reload reused page identity');
 const other=await context.newPage();await other.goto(vite.resolvedUrls.local[0]+'#access');await other.waitForTimeout(300);assert.notEqual(labels.filter(Boolean).at(-1),second,'second page reused identity');
 assert.deepEqual(errors,[]);assert.equal(await page.evaluate(()=>JSON.stringify({...localStorage,...sessionStorage}).includes('fixture-secret')),false);
 writeFileSync(resolve(output,'result.json'),JSON.stringify({pass:true,geometryStable:true,distinctDocuments:true,errors},null,2));console.log('PAGE_KEYS_BROWSER_PASS: fixed metadata rows, 401/success geometry, rotate/revoke, one-time disclosure, fresh reload/tab labels');
}finally{await browser.close();api.closeAllConnections();await new Promise(r=>api.close(r));await vite.close();}

import {chromium} from 'playwright';
import {decode} from 'cborg';
import {readFile,mkdir,writeFile} from 'node:fs/promises';
if(!process.env.LCD7_BASE_URL)throw Error('Set LCD7_BASE_URL to the test device origin');
const base=process.env.LCD7_BASE_URL;
const output=new URL('../../local/multitab/',import.meta.url);await mkdir(output,{recursive:true});
const report={checks:[],tabs:[]},check=(ok,label)=>{if(!ok)throw Error(label);report.checks.push(label);console.log('PASS '+label)};
const browser=await chromium.launch({channel:'msedge',headless:true}),context=await browser.newContext({viewport:{width:1440,height:1000}});
const tabs=[];
async function ready(page){await page.getByTestId('http-state').filter({hasText:'HTTP онлайн'}).waitFor({timeout:30000});await page.getByText('WS онлайн',{exact:true}).waitFor({timeout:30000});await page.getByTestId('sse-count').filter({hasText:/[1-9]/}).waitFor({timeout:30000});}
try{
 for(let attempt=0;attempt<30;attempt++){try{const r=await fetch(base+'/api/status',{signal:AbortSignal.timeout(1000)});if(r.ok)break}catch{}await new Promise(r=>setTimeout(r,500));}
 const expectedExisting=Number(process.env.LCD7_EXISTING_WS||0);let initial;
 for(let attempt=0;attempt<30;attempt++){const response=await fetch(base+'/api/v2/network');initial=decode(new Uint8Array(await response.arrayBuffer()),{useMaps:true});if(Number(initial.get('ws_clients'))>=expectedExisting)break;await new Promise(r=>setTimeout(r,500));}
 check(Number(initial.get('ws_clients'))>=expectedExisting,'existing owner WS accounted before test');
 report.network_before_test=Object.fromEntries([...initial].map(([id,value])=>[id,value]));
 for(let index=0;index<3;index++){
  const page=await context.newPage(),tab={index,opens:0,closes:0,frames:0,boots:[],responses:[],sseStatus:[],pending:[],echoes:[],errors:[],failed:[]};tabs.push({page,tab});report.tabs.push(tab);
  page.on('request',r=>tab.pending.push(r.url()));page.on('requestfinished',r=>{const i=tab.pending.indexOf(r.url());if(i>=0)tab.pending.splice(i,1)});page.on('response',r=>{if(new URL(r.url()).pathname==='/api/v2/events')tab.sseStatus.push(r.status());tab.responses.push([r.status(),r.url()]);if(tab.responses.length>20)tab.responses.shift()});page.on('pageerror',e=>tab.errors.push(String(e)));page.on('requestfailed',r=>tab.failed.push({path:new URL(r.url()).pathname,error:r.failure()?.errorText}));
  page.on('websocket',ws=>{tab.opens++;ws.on('close',()=>tab.closes++);ws.on('framereceived',({payload})=>{try{const m=decode(payload,{useMaps:true});if(m.get('type')==='summary'){tab.frames++;const boot=m.get('boot_id');if(!tab.boots.includes(boot))tab.boots.push(boot);}if(m.get('type')==='echo.response')tab.echoes.push(m.get('data').get('text'));}catch{}})});
  await page.goto(base,{waitUntil:'domcontentloaded',timeout:30000});await ready(page);
 }
 check(true,'three tabs have HTTP + WS + SSE simultaneously');
 for(const {page,tab} of tabs){
  for(const name of ['CPU ↓','Стек ↑','Имя']){await page.getByRole('button',{name,exact:true}).click();const names=await page.locator('.task-table tbody .task-link').allTextContents();check(names[0]==='IDLE0'&&names[1]==='IDLE1',`tab ${tab.index} idle pinned for ${name}`)}
  await page.getByLabel('Поиск задачи',{exact:true}).fill('httpd');const filtered=await page.locator('.task-table tbody .task-link').allTextContents();check(filtered[0]==='IDLE0'&&filtered[1]==='IDLE1'&&filtered.includes('httpd'),`tab ${tab.index} idle remains during search`);await page.getByLabel('Поиск задачи',{exact:true}).fill('');
  await page.getByRole('button',{name:'Лаборатория',exact:true}).click();const message='tab-'+tab.index;await page.getByLabel('Публичный /ws · echo',{exact:true}).fill(message);await page.getByRole('button',{name:'Echo',exact:true}).click();await page.waitForTimeout(300);check(tab.echoes.includes(message),`tab ${tab.index} receives its own echo`);
 }
 const network=await tabs[0].page.evaluate(async()=>new Uint8Array(await(await fetch('/api/v2/network')).arrayBuffer()));report.network_at_peak=Object.fromEntries([...decode(Uint8Array.from(Object.values(network)),{useMaps:true})].map(([id,value])=>[id,value]));check(report.network_at_peak.ws_clients>=expectedExisting+3,'three test WS plus owner WS are active together');check(true,'runtime network metrics read with owner page left open');
 const before=tabs.map(({tab})=>tab.frames);
 for(let reload=0;reload<3;reload++){await tabs[1].page.reload({waitUntil:'domcontentloaded'});await ready(tabs[1].page);check(true,`middle tab reload ${reload+1} restores HTTP/WS/SSE`);}
 // Exercise the retry even when the old SSE slot closes before the new request.
 let rejected=false;await tabs[1].page.route('**/api/v2/events',route=>{if(!rejected){rejected=true;return route.fulfill({status:503,body:'temporary SSE admission full'})}return route.continue()});
 await tabs[1].page.reload({waitUntil:'domcontentloaded'});await ready(tabs[1].page);check(rejected,'SSE recovers from a temporary 503');await tabs[1].page.unroute('**/api/v2/events');
 const sseBefore=await Promise.all(tabs.map(({page})=>page.getByTestId('sse-count').innerText().then(Number)));
 await new Promise(r=>setTimeout(r,15000));
 for(let i=0;i<tabs.length;i++){const {page,tab}=tabs[i];await ready(page);check(Number(await page.getByTestId('sse-count').innerText())>sseBefore[i]+5,'SSE diagnostics advance in tab '+i);check(tab.frames>before[i]+5,`tab ${i} telemetry keeps advancing`);check(tab.boots.length===1,`tab ${i} no device reboot`);check(tab.errors.length===0,`tab ${i} no JavaScript errors`);if(i!==1)check(tab.closes===0&&tab.opens===1,`tab ${i} survives neighbour reload`);}
 const status=await tabs[0].page.evaluate(async()=>(await(await fetch('/api/status')).json()));check(status.ready&&status.touch_errors===0&&status.backlight,'LCD/touch healthy with three tabs');report.status=status;
 await tabs[0].page.screenshot({path:new URL('three-tabs.png',output).pathname.replace(/^\/([A-Za-z]:)/,'$1')});
}catch(error){report.error=String(error);console.error(error);process.exitCode=1}
finally{await writeFile(new URL('report.json',output),JSON.stringify(report,null,2));await browser.close();}

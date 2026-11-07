// Read-only device UI acceptance. Run with Codex CUA: runDeviceUISmoke(tab.playwright).
export async function runDeviceUISmoke(page,onCheck=()=>{},activateTask=()=>page.locator('.task-link').first().click()){
 const checks=[];const check=(v,name)=>{if(!v)throw Error(name);checks.push(name);onCheck(name);};
 const nav=name=>page.getByRole('button',{name,exact:true}).click();
 const text=async id=>(await page.getByTestId(id).textContent())?.trim()??'';
 if((await page.getByRole('button',{name:'RU / EN',exact:true}).textContent()).trim()==='EN')await page.getByRole('button',{name:'RU / EN',exact:true}).click();
 await page.getByTestId('http-state').filter({hasText:'HTTP онлайн'}).waitFor({state:'visible'});
 await page.getByTestId('source-mode').filter({hasText:'ESP32 · устройство'}).waitFor({state:'visible'});
 await page.getByTestId('sse-state').filter({hasText:'SSE онлайн'}).waitFor({state:'visible'});
 check(await page.locator('.simulator').count()===0,'device capability excludes simulator controls');
 await nav('INFO');check((await text('cpu-value')).match(/\d+\.\d+%/)&&!(await text('heap-value')).includes('—'),'real CPU and RAM rendered');
 check((await page.locator('.top-status').textContent()).includes('WS онлайн'),'device WS online alongside SSE');
 const before=await text('sse-count');await page.getByTestId('sse-count').filter({hasText:new RegExp(`^(?!${before}$)`)}).waitFor({state:'visible'});check(true,'native EventSource receives advancing device events');
 check((await page.locator('.details, [data-testid="device-details"]').allTextContents()).some(v=>v.includes('c3-')),'device boot id rendered');
 await nav('Соединения');await page.getByRole('button',{name:'Подробности: Схема обмена',exact:true}).click();await page.locator('dialog[open]').waitFor({state:'visible'});
 check((await page.locator('dialog[open]').textContent()).includes('SSE'),'tooltip module and native dialog work over plain HTTP');
 await page.locator('dialog[open] button').first().press('Escape');await page.locator('dialog[open]').waitFor({state:'hidden'});
 await page.getByRole('button',{name:'Отключить WS',exact:true}).click();const wsOff=await text('sse-count');
 await page.getByTestId('sse-count').filter({hasText:new RegExp(`^(?!${wsOff}$)`)}).waitFor({state:'visible'});check(true,'hardware SSE continues with WS disabled');
 await page.getByRole('button',{name:'Подключить WS',exact:true}).click();await page.getByRole('button',{name:'Отключить SSE',exact:true}).click();const sseOff=await text('sse-count');
 await nav('Лаборатория');await page.getByRole('button',{name:'Echo',exact:true}).click();await page.locator('.frame-log').filter({hasText:'echo.response'}).waitFor({state:'visible'});
 check(await text('sse-count')===sseOff,'hardware WS echo works with SSE disabled');
 await nav('Соединения');await page.getByRole('button',{name:'Подключить SSE',exact:true}).click();await page.getByTestId('sse-state').filter({hasText:'SSE онлайн'}).waitFor({state:'visible'});
 await nav('INFO');await page.locator('.task-link').first().waitFor({state:'visible'});check(await page.locator('.task-link').count()>8,'real FreeRTOS task pages loaded');
 // Both backend task names come from their actual runtime implementations.
 const taskNames=(await page.locator('.task-link').allTextContents()).map(name=>name.trim());
 const httpTaskNames=taskNames.filter(name=>name==='naw_tcp_http'||name==='httpd');
 if(httpTaskNames.length!==1)throw Error('Expected exactly one HTTP backend task');
 await page.getByLabel('Поиск задачи',{exact:true}).fill(httpTaskNames[0]);check(await page.locator('.task-link').count()===1,'real HTTP backend task visible');await activateTask();await page.locator('.task-detail').waitFor({state:'visible'});
 check((await page.locator('.task-detail').textContent()).includes('ticks'),'actual task runtime and stack inspector');await page.getByLabel('Поиск задачи',{exact:true}).fill('');
 await nav('Доступ');await page.getByTestId('auth-verify').click();await page.getByTestId('auth-matrix').locator('tbody tr').nth(4).waitFor({state:'visible'});
 check((await page.getByTestId('auth-matrix').locator('tbody tr').allTextContents()).every(v=>v.includes('PASS')),'device 401 missing / 401 wrong / 200 valid / 403 permissions / 401 private SSE');
 await page.getByTestId('auth-sse').click();await page.getByTestId('auth-matrix').locator('tbody tr').nth(5).waitFor({state:'visible'});
 check((await page.getByTestId('auth-matrix').locator('tbody tr').nth(5).textContent()).includes('PASS'),'authenticated SSE delivered to browser');
 check((await page.locator('.terminal').textContent()).includes('"source":"device"'),'protected SSE has real device source');
 await nav('События / SSE');await page.locator('.event-timeline tbody tr').first().waitFor({state:'visible'});
 check((await page.locator('.event-timeline tbody tr').first().textContent()).includes('c3-'),'device SSE journal populated');
 await appearanceChoice(page,'Тёмная');await page.getByRole('button',{name:'RU / EN',exact:true}).click();await nav('INFO');check((await text('source-mode')).includes('device'),'Dark / English with device source');
 await page.getByRole('button',{name:'RU / EN',exact:true}).click();await appearanceChoice(page,'Кремовая');await nav('INFO');
 check(await page.evaluate(()=>Array.from(document.querySelectorAll('i.ti')).every(i=>!['none','normal','""'].includes(getComputedStyle(i,'::before').content))),'local Tabler glyphs loaded from firmware');
 const result={result:'PASS',checks,visible:{source:await text('source-mode'),cpu:await text('cpu-value'),heap:await text('heap-value'),sseCount:await text('sse-count')}};
 return result;
}

async function appearanceChoice(page,name){await page.getByLabel('Оформление',{exact:true}).click();await page.getByRole('button',{name,exact:true}).click();await page.getByLabel('Оформление',{exact:true}).click();}

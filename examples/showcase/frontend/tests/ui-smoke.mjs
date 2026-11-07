// Browser acceptance uses the same locator surface as Codex CUA.
// In CUA: import this module and call runShowcaseSmoke(tab.playwright).
// Standalone: node tests/ui-smoke.mjs (installed Edge and Playwright).
export async function runShowcaseSmoke(page){
 const checks=[];const check=(value,name)=>{if(!value)throw Error(name);checks.push(name);};
 const nav=name=>page.getByRole('button',{name,exact:true}).click();
 const text=async id=>(await page.getByTestId(id).textContent())?.trim()??'';
 if((await page.getByRole('button',{name:'RU / EN',exact:true}).textContent()).trim()==='EN')await page.getByRole('button',{name:'RU / EN',exact:true}).click();
 await page.getByTestId('http-state').filter({hasText:'HTTP онлайн'}).waitFor({state:'visible'});
 await page.getByTestId('sse-state').filter({hasText:'SSE онлайн'}).waitFor({state:'visible'});
 await nav('INFO');check((await text('source-mode')).includes('Синтетическая'),'explicit synthetic source');
 await appearanceChoice(page,'Кремовая');await appearanceChoice(page,'Ультракомпактная');
 check(await page.evaluate(()=>getComputedStyle(document.body).backgroundColor)==='rgb(253, 251, 247)','TechnicalShell Cream tokens');
 check(await page.evaluate(()=>Array.from(document.querySelectorAll('i.ti')).every(icon=>{const content=getComputedStyle(icon,'::before').content;return content!=='none'&&content!=='normal'&&content!=='""';})),'all rendered Tabler names exist');
 await nav('Соединения');await page.getByRole('button',{name:'Подробности: Схема обмена',exact:true}).click();await page.locator('dialog[open]').waitFor({state:'visible'});
 check(await page.locator('dialog[open]').textContent().then(v=>v.includes('SSE')),'accessible transport explanation');
 await page.locator('dialog[open] button').first().press('Escape');await page.locator('dialog[open]').waitFor({state:'hidden'});
 check(await page.evaluate(()=>document.activeElement?.getAttribute('aria-label')==='Подробности: Схема обмена'),'dialog restores focus');
 const before=await text('sse-count');await page.getByRole('button',{name:'Отключить WS',exact:true}).click();
 await page.getByTestId('sse-count').filter({hasText:new RegExp(`^(?!${before}$)`)}).waitFor({state:'visible'});
 check((await text('sse-state'))==='SSE онлайн','SSE continues with WS disabled');
 await page.getByRole('button',{name:'Подключить WS',exact:true}).click();
 await page.getByRole('button',{name:'Отключить SSE',exact:true}).click();const stopped=await text('sse-count');
 await nav('Лаборатория');await page.getByRole('button',{name:'Echo',exact:true}).click();
 await page.locator('.frame-log').filter({hasText:'echo.response'}).waitFor({state:'visible'});
 check(await text('sse-count')===stopped,'WS echo works with SSE disabled');
 await page.locator('#http-input').fill('Привет & + <script> ✓');await page.getByRole('button',{name:'Отправить',exact:true}).click();
 await page.getByTestId('http-result').filter({hasText:'Привет & + <script> ✓'}).waitFor({state:'visible'});check(true,'safe Unicode query echo');
 await page.getByLabel('HTTP метод',{exact:true}).selectOption('POST');await page.locator('#http-input').fill('form = + & ✓');await page.getByRole('button',{name:'Отправить',exact:true}).click();
 await page.getByTestId('http-result').filter({hasText:'form = + & ✓'}).waitFor({state:'visible'});check(true,'form echo');
 await page.getByRole('button',{name:'Буферизованный ответ',exact:true}).click();await page.getByTestId('http-result').filter({hasText:'Not SSE'}).waitFor({state:'visible'});check(true,'buffered response distinguished from SSE');
 await nav('Доступ');await page.getByTestId('auth-verify').click();await page.getByTestId('auth-matrix').locator('tbody tr').nth(4).waitFor({state:'visible'});
 check((await page.getByTestId('auth-matrix').locator('tbody tr').allTextContents()).every(v=>v.includes('PASS')),'401 missing / 401 wrong / 200 valid / 403 permission / 401 private SSE');
 await page.getByTestId('auth-sse').click();await page.getByTestId('auth-matrix').locator('tbody tr').nth(5).waitFor({state:'visible'});
 check((await page.getByTestId('auth-matrix').locator('tbody tr').nth(5).textContent()).includes('PASS'),'authenticated SSE receives named diagnostic');
 await page.getByRole('button',{name:'Показать WS-соединения',exact:true}).click();await page.locator('.connection-item').waitFor({state:'visible'});check(true,'protected connection list retained');
 await nav('Соединения');await page.getByRole('button',{name:'Подключить SSE',exact:true}).click();await page.getByTestId('sse-state').filter({hasText:'SSE онлайн'}).waitFor({state:'visible'});
 await nav('INFO');await page.locator('.task-link').first().waitFor({state:'visible'});check(await page.locator('.task-link').count()>=10,'task page loads');
 await page.getByLabel('Поиск задачи',{exact:true}).fill('httpd');check(await page.locator('.task-link').count()===1,'task filter');await page.getByRole('button',{name:'httpd',exact:true}).click();await page.getByLabel('Поиск задачи',{exact:true}).fill('');
 await nav('Эксперименты');await page.getByTestId('experiment-kind-pi').click();await page.getByLabel('Длительность, мс',{exact:true}).fill('5000');await page.getByRole('button',{name:'Запустить',exact:true}).click();
 await page.getByTestId('experiment-state').filter({hasText:'Выполняется'}).waitFor({state:'visible'});
 await page.getByTestId('pi-value').filter({hasText:/^3\./}).waitFor({state:'visible'});check(true,'pi progress');
 await page.getByRole('button',{name:'Остановить',exact:true}).click();await page.getByTestId('experiment-state').filter({hasText:'Остановлен'}).waitFor({state:'visible'});
 await page.getByTestId('strip-mode-solid').click();await page.getByTestId('strip-color').fill('#336699');await page.locator('#strip-brightness').fill('50');await page.getByTestId('strip-enabled').setChecked(true);
 await page.getByTestId('strip-state').filter({hasText:'50%'}).waitFor({state:'visible'});check(true,'RGB command/readback');
 await nav('События / SSE');await page.locator('.event-timeline tbody tr').first().waitFor({state:'visible'});check(true,'SSE event log');
 const bounds=await page.evaluate(()=>({body:document.body.scrollHeight,view:innerHeight,width:document.documentElement.scrollWidth,viewport:innerWidth,main:getComputedStyle(document.querySelector('.content-column')).overflowY,sim:getComputedStyle(document.querySelector('.simulator')).overflowY}));
 check(bounds.body<=bounds.view&&bounds.width<=bounds.viewport&&bounds.main==='auto'&&bounds.sim==='auto','fixed shell / separate scroll owners');
 await appearanceChoice(page,'Тёмная');await page.getByRole('button',{name:'RU / EN',exact:true}).click();await nav('INFO');check((await page.getByRole('button',{name:'Details: Source and channels',exact:true}).count())===1,'Dark / English shell');
 await page.getByRole('button',{name:'RU / EN',exact:true}).click();await appearanceChoice(page,'Кремовая');await nav('INFO');
 return {result:'PASS',checks};
}
if(typeof process!=='undefined'&&process.argv[1]&&import.meta.url===new URL('file:///'+process.argv[1].replaceAll('\\','/')).href){
 const {chromium}=await import('playwright');const browser=await chromium.launch({channel:'msedge',headless:true});
 try{const page=await browser.newPage({viewport:{width:1440,height:1000}});await page.goto('http://127.0.0.1:8788/');console.log(JSON.stringify(await runShowcaseSmoke(page),null,2));}finally{await browser.close();}
}

async function appearanceChoice(page,name){await page.getByLabel('Оформление',{exact:true}).click();await page.getByRole('button',{name,exact:true}).click();await page.getByLabel('Оформление',{exact:true}).click();}

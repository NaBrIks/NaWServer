import {chromium} from 'playwright';import {mkdir,writeFile} from 'node:fs/promises';import {fileURLToPath} from 'node:url';
const live=process.env.LCD7_BASE_URL;
const output=new URL(live?'../../local/ui-device/':'../../local/ui-v2/',import.meta.url);await mkdir(output,{recursive:true});
const browser=await chromium.launch({channel:'msedge',headless:true});const report={fixture:!live,checks:[],errors:[]};
const check=(ok,name)=>{if(!ok)throw Error(name);report.checks.push(name);console.log('PASS '+name)};
try{
 const page=await browser.newPage({viewport:{width:1440,height:1000}});page.on('pageerror',e=>report.errors.push(String(e)));
 await page.goto(live||'http://127.0.0.1:8790',{waitUntil:'domcontentloaded'});
 await page.getByTestId('http-state').filter({hasText:'HTTP онлайн'}).waitFor();
 await page.getByText('WS онлайн',{exact:true}).waitFor();check(true,'HTTP and CBOR WebSocket connect');
 await page.getByTestId('sse-count').filter({hasText:/[1-9]/}).waitFor();check(true,'independent SSE receives diagnostics');
 check(await page.getByTestId('tasks-dock').isVisible(),'persistent task dock');
 check(await page.getByText('httpd',{exact:true}).isVisible(),'real task table shape');
 await page.getByRole('button',{name:'Плата и дисплей',exact:true}).click();
 await page.getByTestId('display-swaps').filter({hasText:/[1-9][0-9]*/}).waitFor();check(true,'dedicated board page uses summary data');
 for(const [name,index] of [['Красный',2],['Зелёный',3],['Синий',4],['Сетка',1],['Полосы',0]]){
  await page.getByRole('button',{name,exact:true}).click();
  await page.waitForFunction(async n=>(await(await fetch('/api/status')).json()).pattern===n,index);check(true,'LCD command '+name);
 }
 await page.getByRole('button',{name:'Выключить',exact:true}).click();await page.waitForFunction(async()=>!(await(await fetch('/api/status')).json()).backlight);
 await page.getByRole('button',{name:'Включить',exact:true}).click();await page.waitForFunction(async()=>(await(await fetch('/api/status')).json()).backlight);check(true,'backlight commands');
 await page.getByRole('button',{name:'Подробности: Плата и экран',exact:true}).click();check(await page.getByRole('dialog',{name:'Плата и экран',exact:true}).isVisible(),'help dialog');await page.keyboard.press('Escape');
 for(const theme of ['dark','emerald','light','cream']){await appearanceChoice(page,{dark:'Тёмная',emerald:'Изумрудная',light:'Светлая',cream:'Кремовая'}[theme]);check(await page.locator('html').getAttribute('data-theme')===theme,'theme '+theme)}
 for(const density of ['mini','norm','nano']){await appearanceChoice(page,{mini:'Компактная',norm:'Обычная',nano:'Ультракомпактная'}[density]);check(await page.locator('html').getAttribute('data-density')===density,'density '+density)}
 await page.screenshot({path:fileURLToPath(new URL('board-cream.png',output)),fullPage:true});
 const separator=page.getByRole('separator',{name:'Ширина панели задач',exact:true});const before=Number(await separator.getAttribute('aria-valuenow'));await separator.focus();await page.keyboard.press('ArrowLeft');check(Number(await separator.getAttribute('aria-valuenow'))===before+10,'task dock keyboard resize');
 await page.getByLabel('RU / EN',{exact:true}).click();await page.getByRole('button',{name:'Board and display',exact:true}).waitFor();check(await page.locator('html').getAttribute('lang')==='en','RU/EN switch');await page.getByLabel('RU / EN',{exact:true}).click();
 await page.getByRole('button',{name:'Лаборатория',exact:true}).click();await page.getByLabel('Значение query / form',{exact:true}).fill('S3 · Привет');await page.getByRole('button',{name:'Отправить',exact:true}).click();await page.getByTestId('http-result').filter({hasText:'S3 · Привет'}).waitFor();check(true,'HTTP teaching laboratory preserved');
 await page.getByLabel('Публичный /ws · echo',{exact:true}).fill('S3 WS test');await page.getByRole('button',{name:'Echo',exact:true}).click();await page.getByRole('log').filter({hasText:'echo.response'}).waitFor();check(true,'binary CBOR WS echo preserved');
 await page.getByRole('button',{name:'Эксперименты',exact:true}).click();await page.getByTestId('strip-state').waitFor();check((await page.locator('main').innerText()).includes('GPIO6'),'RGB remapped to GPIO6');
 for(const name of ['Соединения','События / SSE','Доступ','Настройки']){await page.getByRole('button',{name,exact:true}).click();check(await page.locator('h1').innerText()===name,'page '+name)}
 await page.getByRole('button',{name:'Плата и дисплей',exact:true}).click();await appearanceChoice(page,'Тёмная');await page.screenshot({path:fileURLToPath(new URL('board-dark.png',output)),fullPage:true});
 await page.setViewportSize({width:720,height:900});check(await page.locator('main table').first().isVisible(),'table remains a table on narrow viewport');check(await page.locator('main table').first().evaluate(e=>getComputedStyle(e).display==='table'),'no narrow card conversion');await page.screenshot({path:fileURLToPath(new URL('board-narrow.png',output)),fullPage:true});
 check(report.errors.length===0,'no browser JavaScript errors');
}catch(error){report.errors.push(String(error));console.error(error);process.exitCode=1}
finally{await browser.close();await writeFile(new URL('report.json',output),JSON.stringify(report,null,2));}

async function appearanceChoice(page,name){await page.getByLabel('Оформление',{exact:true}).click();await page.getByRole('button',{name,exact:true}).click();await page.getByLabel('Оформление',{exact:true}).click();}

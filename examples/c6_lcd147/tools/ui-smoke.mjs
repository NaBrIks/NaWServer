import assert from 'node:assert/strict';
import {mkdir} from 'node:fs/promises';
import {chromium} from 'playwright';
const base=new URL(process.argv[2]);assert.equal(base.protocol,'http:');
const output=new URL('../local/',import.meta.url);await mkdir(output,{recursive:true});
const browser=await chromium.launch({headless:true,args:['--no-proxy-server'],...(process.env.C6_BROWSER_EXECUTABLE?{executablePath:process.env.C6_BROWSER_EXECUTABLE}:{channel:'msedge'})});
try{
 const page=await browser.newPage({viewport:{width:1440,height:1000}});
 const errors=[];page.on('pageerror',error=>errors.push(String(error)));
 await page.goto(base.href,{waitUntil:'domcontentloaded'});
 await page.getByTestId('http-state').filter({hasText:/HTTP (онлайн|online)/}).waitFor();
 await page.getByTestId('sse-state').filter({hasText:/SSE (онлайн|online)/}).waitFor();
 await page.locator('.top-status').filter({hasText:/WS (онлайн|online)/}).waitFor();
 await page.locator('.task-link').first().waitFor();
 assert((await page.locator('.brand').textContent()).includes('C6 LCD 1.47'));
 assert.equal(await page.locator('.page-strip h1').textContent(),'INFO');
 await page.getByTestId('device-details').waitFor();
 const measurements=await page.evaluate(()=>({chart:document.querySelector('.task-history').getBoundingClientRect().height,row:document.querySelector('.task-table tbody tr').getBoundingClientRect().height}));
 assert(Math.abs(measurements.chart-measurements.row*3)<2,JSON.stringify(measurements));
 assert.equal(await page.locator('.task-history svg text').count(),0);
 assert.equal(await page.locator('.task-history .chart-grid path').count(),10);
 assert((await page.locator('[data-field="wifi_sta_mac"]').textContent()).includes('E4:B0:63'));
 console.log('PASS INFO / compact grids / chart height = 3 table rows: '+JSON.stringify(measurements));
 for(const density of ['mini','norm','nano']){
  const sizes=await page.evaluate(density=>{document.documentElement.dataset.density=density;return {chart:document.querySelector('.task-history').getBoundingClientRect().height,row:document.querySelector('.task-table tbody tr').getBoundingClientRect().height};},density);
  assert(Math.abs(sizes.chart-sizes.row*3)<2,density+JSON.stringify(sizes));
 }
 await page.screenshot({path:new URL('overview.png',output).pathname.replace(/^\/(\w:)/,'$1'),fullPage:true});
 await page.locator('.content-column').evaluate(element=>{element.scrollTop=element.scrollHeight;});
 await page.locator('[data-detail-group="build"]').waitFor();
 assert(await page.locator('[data-field="ieee802154_eui64"]').evaluate(cell=>cell.scrollWidth<=cell.clientWidth),'EUI-64 must fit without clipping');
 await page.screenshot({path:new URL('info-details.png',output).pathname.replace(/^\/(\w:)/,'$1'),fullPage:true});
 await page.locator('.content-column').evaluate(element=>{element.scrollTop=0;});
 await page.getByRole('button',{name:/^(Эксперименты|Experiments)$/}).click();
 await page.getByTestId('strip-state').waitFor();
 assert.equal(await page.locator('.rgb-strip>span').count(),1);
 assert.equal(await page.getByTestId('strip-driver-rmt').count(),1);
 assert.equal(await page.getByTestId('strip-driver-spi').count(),0);
 assert((await page.locator('.panel-head').allTextContents()).some(text=>text.includes('GPIO8')));
 await page.screenshot({path:new URL('rgb.png',output).pathname.replace(/^\/(\w:)/,'$1'),fullPage:true});
 assert.deepEqual(errors,[]);
 console.log('PASS real C6 browser: HTTP + WS + SSE, project label, GPIO8 / one RGB / RMT only, no JS errors');
}finally{await browser.close();}

import assert from 'node:assert/strict';import {mkdir} from 'node:fs/promises';import {chromium} from 'playwright';import {fileURLToPath} from 'node:url';
const output=new URL('../../local/info-preview/',import.meta.url);await mkdir(output,{recursive:true});
const browser=await chromium.launch({headless:true,args:['--no-proxy-server'],executablePath:process.env.C6_BROWSER_EXECUTABLE});
try{
 const page=await browser.newPage({viewport:{width:2000,height:1200}});const errors=[];page.on('pageerror',error=>errors.push(String(error)));
 await page.goto('http://127.0.0.1:8792',{waitUntil:'domcontentloaded'});
 await page.getByTestId('http-state').filter({hasText:/HTTP (онлайн|online)/}).waitFor();await page.locator('.task-link').first().waitFor();
 await page.locator('[data-core="CPU1"]').waitFor({state:'attached'});await page.getByTestId('device-details').waitFor();
 await page.getByTestId('sse-state').filter({hasText:/SSE (онлайн|online)/}).waitFor();
 await page.waitForFunction(()=>[...document.querySelectorAll('.task-history svg>path')].every(path=>path.getAttribute('d')?.includes('L')));
 assert.equal(await page.locator('.resource-charts svg.chart').count(),2);assert((await page.locator('.resource-charts .chart-axis text').count())>=10);
 assert.equal(await page.locator('.tasks-dock [data-testid="task-history"]').count(),1);assert.equal(await page.locator('.task-history svg>path').count(),3);assert.equal(await page.locator('.task-history svg text').count(),0);
 const geometry=()=>page.evaluate(()=>{const graph=document.querySelector('.task-history'),table=document.querySelector('.task-table'),row=table.querySelector('tbody tr');return {height:graph.getBoundingClientRect().height,row:row.getBoundingClientRect().height,bottom:graph.getBoundingClientRect().bottom,tableTop:table.getBoundingClientRect().top,columns:String(document.querySelectorAll('.device-detail-column').length),colors:[...document.querySelectorAll('.task-history figcaption span')].map(span=>({name:span.textContent,color:getComputedStyle(span).color}))};});
 for(const [width,columns] of [[2000,'3'],[2560,'4']]){
  await page.setViewportSize({width,height:1200});await page.waitForFunction(expected=>document.querySelectorAll('.device-detail-column').length===Number(expected),columns);const layout=await geometry();assert.equal(layout.columns,columns);assert(Math.abs(layout.height-layout.row*3)<1,JSON.stringify(layout));assert(layout.bottom<=layout.tableTop+1);assert.equal(layout.colors.length,3);assert.notEqual(layout.colors[0].color,layout.colors[1].color);assert.notEqual(layout.colors[1].color,layout.colors[2].color);
  await page.screenshot({path:fileURLToPath(new URL(`info-${columns}-columns.png`,output)),fullPage:true});console.log('PASS '+JSON.stringify({width,...layout}));
 }
 const before=await geometry();await page.locator('.content-column').evaluate(element=>{element.scrollTop=element.scrollHeight;});const after=await geometry();assert.equal(after.bottom,before.bottom);
 await page.screenshot({path:fileURLToPath(new URL('info-bottom.png',output)),fullPage:true});
 assert.deepEqual(errors,[]);console.log('PASS restored INFO graphs, combined CPU0/CPU1/MEM above task table, independent scrolling, emulator only');
}finally{await browser.close();}

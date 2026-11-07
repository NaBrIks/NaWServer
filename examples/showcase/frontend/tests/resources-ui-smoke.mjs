// CUA: runResourcesSmoke(tab.playwright, {activateTask: native AX callback}).
export async function runResourcesSmoke(page,{activateTask=()=>page.locator('.task-link').first().click()}={}){
 const checks=[];const check=(value,name)=>{if(!value)throw Error(name);checks.push(name);};
 await page.getByRole('heading',{name:'INFO',exact:true}).waitFor({state:'visible'});
 await page.locator('.task-link').nth(4).waitFor({state:'visible',timeoutMs:10000});
 check(await page.getByRole('button',{name:'Память',exact:true}).count()===0&&await page.getByRole('button',{name:'Задачи / CPU',exact:true}).count()===0,'one resource page instead of separate CPU / memory / tasks pages');
 check(await page.getByTestId('resource-charts').locator('svg.chart').count()===2&&await page.getByTestId('memory-pools').count()===1&&await page.getByTestId('tasks-panel').count()===1,'CPU and memory charts, all memory pools and tasks coexist');
 const cpu=page.getByTestId('resource-charts').locator('svg.chart').nth(0),ram=page.getByTestId('resource-charts').locator('svg.chart').nth(1),miniRam=page.locator('.mini-trend').nth(1);
 const capacity=Number(await ram.getAttribute('data-max'));
 check(await cpu.getAttribute('data-min')==='0'&&await cpu.getAttribute('data-max')==='100','CPU fixed 0–100% scale');
 check(await ram.getAttribute('data-min')==='0'&&capacity>0&&Number(await miniRam.getAttribute('data-max'))===capacity&&await miniRam.getAttribute('data-min')==='0','full and miniature RAM charts share fixed zero-to-capacity scale');
 const tableCapacity=Number((await page.locator('.memory-table tbody tr').first().locator('td').nth(1).textContent()).replace(/\s/g,'').replace(',','.'));
 check(Math.abs(capacity-tableCapacity)<0.1,'RAM scale uses physical pool capacity, not recent extrema');
 const alignment=await page.evaluate(()=>{
  const cells=Array.from(document.querySelectorAll('.task-table .numeric,.memory-table .numeric')).filter(cell=>!cell.querySelector('.task-cpu-cell'));
  cells.push(...document.querySelectorAll('.task-cpu-cell>span:first-child'));
  const range=document.createRange();
  const right=cells.every(cell=>{range.selectNodeContents(cell);const style=getComputedStyle(cell);return style.textAlign==='right'&&style.fontVariantNumeric.includes('tabular-nums')&&Math.abs(range.getBoundingClientRect().right-(cell.getBoundingClientRect().right-Number(style.paddingRight.replace('px',''))))<1;});
  const readable=Array.from(document.querySelectorAll('.task-table th,.memory-table th')).every(cell=>{range.selectNodeContents(cell);const style=getComputedStyle(cell);return range.getBoundingClientRect().width<=cell.getBoundingClientRect().width-Number(style.paddingLeft.replace('px',''))-Number(style.paddingRight.replace('px',''))+1;});
  const noOverflow=Array.from(document.querySelectorAll('.task-table,.memory-table')).every(table=>table.parentElement.scrollWidth<=table.parentElement.clientWidth+1||getComputedStyle(table.parentElement).overflowX==='auto');
  return {right,readable,noOverflow};
 });
 check(alignment.right,'numeric headers and every value align to the same right edge with tabular figures');
 check(alignment.readable,'numeric headers are fully visible');
 check(alignment.noOverflow,'resource tables fit the desktop work area');
 await page.getByLabel('Поиск задачи',{exact:true}).fill('httpd');check(await page.locator('.task-link').count()===1,'task filtering remains available');
 await activateTask();await page.locator('.task-detail').waitFor({state:'visible'});
 check((await page.locator('.task-detail').textContent()).includes('ticks')&&await page.locator('.task-inspector svg.chart').getAttribute('data-max')==='100','task runtime, stack and fixed-scale CPU history remain on the same page');
 await page.getByRole('button',{name:'Закрыть подробности задачи',exact:true}).click();check(await page.locator('.task-inspector').count()===0,'task inspector closes independently');
 await page.getByLabel('Поиск задачи',{exact:true}).fill('');await page.locator('.task-link').nth(4).waitFor({state:'visible'});
 const count=(await page.getByTestId('sse-count').textContent()).trim();await page.getByTestId('sse-count').filter({hasText:new RegExp(`^(?!${count}$)`)}).waitFor({state:'visible'});
 check(Number(await miniRam.getAttribute('data-max'))===capacity&&await cpu.getAttribute('data-max')==='100','telemetry advances without rescaling');
 check((await page.locator('.top-status').textContent()).includes('WS онлайн')&&(await page.locator('.top-status').textContent()).includes('SSE онлайн'),'WS and SSE remain online on the combined page');
 await page.locator('.content-column').press('Control+Home');
 return {result:'PASS',checks,capacityKiB:capacity,taskCount:await page.locator('.task-link').count()};
}

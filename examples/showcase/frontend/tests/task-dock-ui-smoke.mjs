// Run via CUA; activate resolves navigation from fresh native AX state.
export async function runTaskDockSmoke(page,{activate,reload}={}){
 const checks=[];const check=(value,name)=>{if(!value)throw Error(name);checks.push(name);};
 const nav=async name=>{await (activate?activate(name):page.getByRole('button',{name,exact:true}).click());await page.getByRole('heading',{name,exact:true}).waitFor({state:'visible'});};
 const geometry=()=>page.evaluate(()=>{const d=document.querySelector('.tasks-dock').getBoundingClientRect(),h=document.querySelector('.topbar').getBoundingClientRect();return {left:d.left,right:d.right,top:d.top,bottom:d.bottom,width:d.width,viewWidth:innerWidth,viewHeight:innerHeight,headerBottom:h.bottom,bodyHeight:document.body.scrollHeight,bodyWidth:document.documentElement.scrollWidth};});
 await page.locator('.task-link').nth(4).waitFor({state:'visible',timeoutMs:10000});
 const baseline=await geometry();
 check(baseline.right===baseline.viewWidth&&baseline.top===baseline.headerBottom&&baseline.bottom===baseline.viewHeight,'task dock spans the right edge below the common header');
 for(const name of ['INFO','Соединения','Лаборатория','Эксперименты','События / SSE','Доступ','Настройки']){
  await nav(name);const box=await geometry();check(box.top===baseline.top&&box.right===baseline.right&&box.bottom===baseline.bottom&&await page.getByTestId('tasks-panel').count()===1,`${name}: same pinned task panel`);
 }
 const historyBefore=await page.locator('.task-table tbody tr').first().locator('.mini-line').getAttribute('d');
 for(let i=0;i<3;i++){const count=(await page.getByTestId('sse-count').textContent()).trim();await page.getByTestId('sse-count').filter({hasText:new RegExp(`^(?!${count}$)`)}).waitFor({state:'visible'});}
 check(await page.locator('.task-table tbody tr').first().locator('.mini-line').getAttribute('d')!==historyBefore,'process history keeps updating while Settings is open');
 check(await page.locator('.task-state-icon').count()===await page.locator('.task-link').count(),'one state icon per task');
 check((await page.locator('.task-state-icon').evaluateAll(nodes=>nodes.every(n=>n.getAttribute('aria-label')&&n.getAttribute('data-tooltip')&&!n.textContent.trim()))),'states have accessible names and tooltips without repeated text');
 check(await page.locator('.task-table tbody .mini-trend').count()===await page.locator('.task-link').count(),'one CPU mini chart per task');
 check(await page.locator('.task-table .mini-trend').evaluateAll(nodes=>nodes.every(n=>n.getAttribute('data-min')==='0'&&n.getAttribute('data-max')==='100')),'every process mini chart stays at 0–100%');
 check(await page.locator('.task-table').evaluate(el=>Array.from(el.querySelectorAll('th')).slice(1).every(th=>th.getAttribute('data-tooltip')||th.querySelector('[data-tooltip]'))),'short headers retain full explanations');
 const rows=await page.locator('.task-table tbody tr').evaluateAll(nodes=>nodes.map(n=>n.getBoundingClientRect().height));
 check(Math.max(...rows)-Math.min(...rows)<1&&Math.max(...rows)<=26,'icons and charts preserve equal compact row heights');
 check(await page.getByTestId('tasks-panel').locator('.table-wrap').evaluate(el=>getComputedStyle(el).overflowY==='auto'),'task table owns its scrolling');
 const splitter=page.getByRole('separator',{name:'Ширина панели задач',exact:true});
 const initial=Number(await splitter.getAttribute('aria-valuenow')),maximum=Number(await splitter.getAttribute('aria-valuemax'));
 const grow=maximum-initial>=10;await splitter.press(grow?'ArrowLeft':'ArrowRight');
 const changed=Number(await splitter.getAttribute('aria-valuenow'));check(changed===initial+(grow?10:-10),'keyboard divider changes width by 10 px');
 check((await geometry()).width===changed,'grid uses actual requested dock width');
 if(reload){await reload();check(Number(await splitter.getAttribute('aria-valuenow'))===changed,'dock width persists across reload');}
 await splitter.press(grow?'ArrowRight':'ArrowLeft');check(Number(await splitter.getAttribute('aria-valuenow'))===initial,'initial panel width restored');
 await nav('INFO');await page.locator('.content-column').press('Control+End');
 const scrolled=await geometry();check(scrolled.top===baseline.top&&scrolled.bottom===baseline.bottom,'scrolling the overview does not move tasks');
 await page.locator('.content-column').press('Control+Home');
 check(scrolled.bodyHeight<=scrolled.viewHeight&&scrolled.bodyWidth<=scrolled.viewWidth,'shell has no outer overflow');
 return {result:'PASS',checks,width:initial,rowHeight:Math.max(...rows)};
}

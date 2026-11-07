// Run through CUA with runShellSmoke(tab.playwright, {simulator:true,onErrorShown}).
export async function runShellSmoke(page,{simulator=false,onErrorShown=async()=>{},activate}={}){
 const checks=[];const check=(value,name)=>{if(!value)throw Error(name);checks.push(name);};
 const nav=async name=>{await (activate?activate(name):page.getByRole('button',{name,exact:true}).click());await page.getByRole('heading',{name,exact:true}).waitFor({state:'visible'});};
 const geometry=()=>page.evaluate(()=>{const m=document.querySelector('.metrics').getBoundingClientRect(),h=document.querySelector('.page-strip').getBoundingClientRect(),c=document.querySelector('.content-grid').getBoundingClientRect();return {metricsTop:m.top,metricsBottom:m.bottom,headingTop:h.top,contentTop:c.top,bodyHeight:document.body.scrollHeight,view:innerHeight,width:document.documentElement.scrollWidth,viewport:innerWidth};});
 await page.getByTestId('http-state').filter({hasText:'HTTP онлайн'}).waitFor({state:'visible',timeoutMs:10000});
 for(const name of ['INFO','Соединения','Лаборатория','Эксперименты','События / SSE','Доступ','Настройки']){
  await nav(name);const box=await geometry();
  check(box.metricsBottom<=box.headingTop&&await page.getByTestId('global-metrics').locator('.mini-trend').count()===2,`${name}: permanent metrics above heading and two mini charts`);
  check(box.bodyHeight<=box.view&&box.width<=box.viewport,`${name}: shell stays inside viewport`);
  check(await page.locator('.transport-diagram').count()===(name==='Соединения'?1:0),`${name}: transport diagram has one owner`);
 }
 check(await page.getByRole('button',{name:'Отключить WS',exact:true}).count()===0,'interface settings do not duplicate connection controls');
 await nav('Эксперименты');const before=await geometry();
 await nav('Доступ');const password=page.getByLabel('Пароль',{exact:true});await password.fill('showcase-only-wrong');await nav('Эксперименты');
 try{
  await page.getByRole('button',{name:'Запустить',exact:true}).click();
  await page.locator('.notification.error').waitFor({state:'visible',timeoutMs:10000});
  check((await page.locator('.notification.error').textContent()).includes('401'),'real command failure opens red error notification');
  const after=await geometry();check(before.contentTop===after.contentTop&&before.headingTop===after.headingTop,'error overlay does not shift page');
  check(await page.getByTestId('notifications').evaluate(el=>getComputedStyle(el).position)==='fixed','notification stack is fixed');
  check(await page.locator('.notification.error progress').count()===0,'errors stay until explicitly dismissed');
  await onErrorShown();
  await page.getByRole('button',{name:'Запустить',exact:true}).click();
  await page.getByRole('button',{name:'Запустить',exact:true}).waitFor({state:'visible'});
  check(await page.locator('.notification.error').count()===1,'identical command errors do not stack');
  if(simulator){
   await page.locator('.simulator-tools>summary').click();
   await page.getByRole('button',{name:'Обычный ритм Стабильная память',exact:true}).click();
   await page.locator('.notification.success').waitFor({state:'visible'});
   check(await page.locator('.notification.success progress').count()===1,'success has visible dismissal timer');
   // CUA may bound a single locator wait below the application timer. Retry only
   // while the observed countdown is advancing, with a strict overall bound.
   let remaining=Infinity;
   for(let attempt=0;attempt<3;attempt++){
    try{await page.locator('.notification.success').waitFor({state:'hidden',timeoutMs:10000});break;}
    catch(error){const value=await page.locator('.notification.success progress').getAttribute('value');if(value===null)break;const current=Number(value);if(attempt===2||current>=remaining)throw error;remaining=current;}
   }
   check(await page.locator('.notification.error').isVisible(),'success expires while error remains');
  }
  await page.getByTestId('notifications-close-all').click();
  check(await page.getByTestId('notifications').count()===0,'Close all dismisses the complete stack');
 }finally{await nav('Доступ');await password.fill('showcase-only');await nav('Эксперименты');}
 await appearanceChoice(page,'Тёмная');await page.getByRole('button',{name:'RU / EN',exact:true}).click();
 check(await page.getByRole('heading',{name:'Experiments',exact:true}).count()===1,'Dark / English keeps a single page heading');
 await page.getByRole('button',{name:'RU / EN',exact:true}).click();await appearanceChoice(page,'Кремовая');
 return {result:'PASS',checks};
}

async function appearanceChoice(page,name){await page.getByLabel('Оформление',{exact:true}).click();await page.getByRole('button',{name,exact:true}).click();await page.getByLabel('Оформление',{exact:true}).click();}

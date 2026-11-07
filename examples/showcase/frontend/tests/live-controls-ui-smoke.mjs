// CUA acceptance on the simulator or authorized C3: runLiveControlsSmoke(tab.playwright).
// Restores the initial RGB settings after exercising the live controls.
export async function runLiveControlsSmoke(page){
 const checks=[];const check=(value,name)=>{if(!value)throw Error(name);checks.push(name);};
 check(await page.getByTestId('experiment-kind').locator('button').count()===2,'two separate workload mode buttons');
 check(await page.getByTestId('experiment-kind').locator('select').count()===0,'no workload mode dropdown');
 await page.getByTestId('experiment-kind-pulse').click();
 check(await page.getByLabel('Активная часть, мс',{exact:true}).isEnabled(),'pulse button enables pulse settings');
 await page.getByTestId('experiment-kind-pi').click();
 check(!await page.getByLabel('Активная часть, мс',{exact:true}).isEnabled(),'pi button selects pi settings without starting workload');
 const state=page.getByTestId('strip-state');
 const applied=async text=>state.filter({hasText:text}).waitFor({state:'visible',timeoutMs:10000});
 const initial=await page.evaluate(()=>({
  enabled:document.querySelector('[data-testid="strip-enabled"]').checked,
  mode:document.querySelector('[data-testid="strip-mode"] button[aria-pressed="true"]').dataset.testid.replace('strip-mode-',''),
  color:document.querySelector('[data-testid="strip-color"]').value,
  brightness:document.querySelector('#strip-brightness').value,
  period:document.querySelector('[data-testid="strip-period"]').value
 }));
 check(await page.getByTestId('strip-mode').locator('button').count()===3,'three effect buttons');
 check(await page.getByTestId('strip-mode').locator('select').count()===0,'no effect dropdown');
 check(await page.getByRole('button',{name:/Применить RGB|Apply RGB/}).count()===0,'no Apply RGB button');
 try{
  await page.locator('#strip-brightness').fill('10');await applied('10%');check(true,'brightness applies without submit');
  await page.getByTestId('strip-enabled').setChecked(false);await applied('выключено');
  await page.getByTestId('strip-enabled').setChecked(true);await applied('включено');check(true,'power toggles apply immediately');
  await page.getByTestId('strip-mode-rainbow').click();await applied('rainbow');
  check(await page.getByTestId('strip-mode-rainbow').getAttribute('aria-pressed')==='true','rainbow button applies and highlights');
  await page.getByTestId('strip-period').fill('150');await applied('150 ms');check(true,'period applies without submit');
  await page.locator('#strip-brightness').fill('11');await page.locator('#strip-brightness').fill('12');await page.locator('#strip-brightness').fill('10');
  await applied('10%');check(await page.locator('#strip-brightness').isEnabled(),'latest slider value applies and controls stay enabled');
  await page.getByTestId('strip-mode-chase').click();await applied('chase');check(true,'chase button applies');
  await page.getByTestId('strip-mode-solid').click();await page.getByTestId('strip-color').fill('#336699');
  await page.locator('.rgb-strip span[data-color="#050a0f"]').nth(9).waitFor({state:'visible'});
  check(await page.locator('.rgb-strip span[data-color="#050a0f"]').count()===10,'solid color and actual scaled pixel readback');
  await page.getByTestId('strip-live-status').filter({hasText:'Живое управление'}).waitFor({state:'visible'});
  check((await page.locator('.top-status').textContent()).includes('WS онлайн')&&(await page.locator('.top-status').textContent()).includes('SSE онлайн'),'WS and SSE stay online during live controls');
 }finally{
  await page.getByTestId('strip-mode-solid').click();await page.getByTestId('strip-color').fill(initial.color);
  await page.locator('#strip-brightness').fill(initial.brightness);
  await page.getByTestId('strip-mode-chase').click();await page.getByTestId('strip-period').fill(initial.period);
  await page.getByTestId('strip-mode-'+initial.mode).click();await page.getByTestId('strip-enabled').setChecked(initial.enabled);
  await applied(`${initial.mode} · ${initial.brightness}% · ${initial.period} ms`);
  await page.getByTestId('strip-live-status').filter({hasText:'Живое управление'}).waitFor({state:'visible'});
 }
 check(true,'initial RGB settings restored');
 return {result:'PASS',checks,readback:await state.textContent()};
}

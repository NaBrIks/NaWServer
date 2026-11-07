// Run inside CUA against the model or authorized C3. Leaves RMT / RGB on.
export async function runRgbDriversSmoke(page,activateDriver=id=>page.getByTestId('strip-driver-'+id).click()){
 const check=(ok,name)=>{if(!ok)throw Error(name);};
 const read=()=>page.getByTestId('strip-state').innerText();
 const pixels=()=>page.locator('.rgb-strip span').evaluateAll(nodes=>nodes.map(n=>n.getAttribute('data-color')));
 check(await page.getByTestId('strip-driver').locator('button').count()===2,'two separate driver buttons');
 await page.getByTestId('strip-mode-solid').click();
 await page.getByTestId('strip-color').fill('#ffa030');
 await page.locator('#strip-brightness').fill('20');
 await page.getByTestId('strip-enabled').setChecked(true);
 const reports=[];
 for(let i=0;i<10;i++){
  const driver=i%2?'rmt':'spi';
  await activateDriver(driver);
  await page.getByTestId('strip-state').filter({hasText:driver.toUpperCase()+' · включено · solid · 20%'}).waitFor({state:'visible'});
  await page.getByTestId('strip-driver-'+driver).and(page.locator('[aria-pressed="true"]')).waitFor({state:'visible'});
  check((await pixels()).every(c=>c==='#332009'),'all ten pixels preserve the same frame after switch');
  check((await pixels()).length===10,'ten pixel slots');
  check(!(await page.getByTestId('strip-live-status').innerText()).includes('ESP_ERR'),'no driver allocation/output error');
  reports.push(await read());
 }
 check((await page.locator('.top-status').innerText()).includes('WS онлайн')&&(await page.locator('.top-status').innerText()).includes('SSE онлайн'),'WS and SSE online after ten switches');
 return {result:'PASS',switches:reports.length,final:await read()};
}

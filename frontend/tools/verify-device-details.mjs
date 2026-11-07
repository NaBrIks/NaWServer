import assert from 'node:assert/strict';
import {readFile,mkdir} from 'node:fs/promises';
import {decode,encode} from 'cborg';
import {chromium} from 'playwright';
const directory=new URL('../../local/device-info-tests/',import.meta.url);await mkdir(directory,{recursive:true});
const browser=await chromium.launch({headless:true,args:['--no-proxy-server'],executablePath:process.env.C6_BROWSER_EXECUTABLE});
try{
 for(const target of ['c3','s3','c6']){
  const details=decode(new Uint8Array(await readFile(new URL(`${target}-connected.cbor`,directory))));
  const page=await browser.newPage({viewport:{width:2560,height:1200}}),errors=[];
  page.on('pageerror',error=>errors.push(String(error)));
  await page.route('**/api/v2/info',async route=>{
   const response=await route.fetch(),info=decode(new Uint8Array(await response.body()));info.details=details;
   await route.fulfill({response,body:Buffer.from(encode(info))});
  });
  await page.goto('http://127.0.0.1:8792',{waitUntil:'domcontentloaded'});
  await page.getByTestId('device-details').waitFor();
  await page.waitForFunction(()=>document.querySelectorAll('.device-detail-column').length===4);
  assert.equal(await page.locator('[data-detail-group]').count(),6);
  assert.equal(await page.locator('[data-field="chip_model"]').innerText(),`ESP32-${target.toUpperCase()}`);
  assert.equal(await page.locator('[data-field="rgb_gpio"]').innerText(),String({c3:2,s3:6,c6:8}[target]));
  if(target==='s3')assert.equal(await page.locator('[data-field="touch_controller"]').innerText(),'GT911');
  if(target==='c3')assert.equal(await page.locator('[data-field="lcd_resolution"]').count(),0);
  assert.deepEqual(errors,[]);
  await page.screenshot({path:new URL(`${target}-info.png`,directory).pathname.replace(/^\/([A-Z]:)/,'$1')});
  console.log(`PASS ${target}: native collector CBOR rendered in six shared groups / four columns, board fields, no JS errors`);
  await page.close();
 }
}finally{await browser.close();}

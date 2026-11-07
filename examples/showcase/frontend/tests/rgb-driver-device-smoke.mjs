import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {encode,decode} from 'cborg';
const unpack=v=>v instanceof Map?Object.fromEntries([...v].map(([k,x])=>[k,unpack(x)])):Array.isArray(v)?v.map(unpack):v;
const url=process.argv[2];if(!url)throw Error('Explicit authorized device URL required');
async function api(path,payload){
 const response=await fetch(new URL(path,url),{signal:AbortSignal.timeout(4000),...(payload?{method:'POST',headers:{'Content-Type':'application/cbor',Authorization:'Basic '+Buffer.from('demo:showcase-only').toString('base64')},body:encode(new Map(Object.entries({v:2,...payload}).map(([k,v])=>[k,v])))}:{})});
 const data=unpack(decode(new Uint8Array(await response.arrayBuffer()),{useMaps:true}));
 assert.equal(response.status,200,`${path}: ${response.status}`);return data;
}
const initial=await api('/api/v2/strip');
if(process.argv[3]==='--inspect'){console.log(JSON.stringify(initial,null,2));process.exit(0);}
const command={enabled:true,mode:initial.mode,color:initial.color,brightness_pct:initial.brightness_pct,period_ms:initial.period_ms};
const before=await api('/api/v2/summary');const checks=[];
try{
 for(let i=0;i<10;i++){
  const driver=i%2?'rmt':'spi';await api('/api/v2/strip',{...command,driver});
  let applied;const deadline=performance.now()+8000;
  do{applied=await api('/api/v2/strip');if(applied.driver===driver&&applied.requested_driver===driver)break;}while(performance.now()<deadline);
  assert.equal(applied.driver,driver);assert.equal(applied.requested_driver,driver);
  assert.equal(applied.driver_error,'');assert.equal(applied.supported,true);assert.equal(applied.enabled,true);
  assert.equal(applied.count,10);assert.equal(applied.pixels.length,10);
  assert.equal(applied.color,initial.color);assert.equal(applied.brightness_pct,initial.brightness_pct);
  checks.push(driver);console.log(`PASS ${i+1}/10: ${driver}, supported, enabled, ten pixels, no driver error`);
 }
 const after=await api('/api/v2/summary');assert.equal(after.boot_id,before.boot_id,'driver switches must not reboot the board');
 const report={result:'PASS',switches:checks.length,boot_unchanged:true,final:await api('/api/v2/strip')};
 if(process.argv[3])await writeFile(process.argv[3],JSON.stringify(report,null,2));
 console.log('PASS: ten real driver switches without reboot; original color/brightness preserved; final RMT/on');
}finally{await api('/api/v2/strip',{...command,driver:'rmt'});}

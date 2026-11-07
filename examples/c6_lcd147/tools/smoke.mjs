// Explicit hardware URL required. Read-only, apart from WebSocket echo.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {encodeWire,decodeWire} from '../../../frontend/src/contracts/cbor.ts';
import {isSummary,isStrip} from '../../../frontend/src/contracts/index.ts';
const base=new URL(process.argv[2]);
assert.equal(base.protocol,'http:');
const get=async path=>{
 const response=await fetch(new URL(path,base),{signal:AbortSignal.timeout(8000)});
 assert.equal(response.status,200,path);
 return decodeWire(new Uint8Array(await response.arrayBuffer()));
};
const info=await get('/api/v2/info');assert.equal(info.chip.model,'ESP32-C6');assert.equal(info.source,'device');
assert.equal(info.details.hardware.chip_model,'ESP32-C6');assert.equal(info.details.hardware.cpu_mhz,160);
assert.equal(info.details.flash_memory.flash_bytes,4*1024*1024);
assert.match(info.details.mac_addresses.wifi_sta_mac,/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/);
assert.match(info.details.mac_addresses.ieee802154_eui64,/^([0-9A-F]{2}:){7}[0-9A-F]{2}$/);
assert.equal(info.details.board.rgb_order,'RGB');assert.equal(info.details.board.touch,false);
assert.equal(info.details.board.lcd_spi_ready,true);assert.equal(info.details.board.lcd_visual_status,'unverified');
assert.match(info.details.build.elf_sha256,/^[0-9a-f]{64}$/);
console.log('PASS actual ESP32-C6 identity');
const summary=await get('/api/v2/summary');assert(isSummary(summary.data));assert(summary.data.memory[0].free_bytes>0);
const strip=await get('/api/v2/strip');assert(isStrip(strip));assert.equal(strip.gpio,8);assert.equal(strip.count,1);assert.deepEqual(strip.available_drivers,['rmt']);
console.log('PASS RAM, shared telemetry contract, onboard GPIO8 / one pixel / RMT');
const tasks=await get('/api/v2/tasks?offset=0&limit=8');assert(tasks.data.items.length>0);
console.log('PASS FreeRTOS task telemetry');
const manifest=JSON.parse(await readFile(new URL('../frontend/dist/device-manifest.json',import.meta.url),'utf8'));
for(const asset of manifest.files){
 const response=await fetch(new URL(asset.path==='index.html'?'/':'/'+asset.path,base),{signal:AbortSignal.timeout(8000)});
 assert.equal(response.status,200);assert.equal(createHash('sha256').update(new Uint8Array(await response.arrayBuffer())).digest('hex'),asset.sha256,asset.path);
}
console.log(`PASS ${manifest.files.length} embedded frontend assets (SHA256)`);
await new Promise((resolve,reject)=>{
 const socket=new WebSocket(new URL('/ws',base).href.replace('http:','ws:'));socket.binaryType='arraybuffer';
 const timeout=setTimeout(()=>{socket.close();reject(Error('WS timeout'));},8000);
 socket.onopen=()=>socket.send(encodeWire({v:2,type:'echo.request',request_id:'c6-smoke',data:{bytes:new Uint8Array([0,128,255,0])}}));
 socket.onmessage=event=>{
  try{const message=decodeWire(new Uint8Array(event.data));if(message.type!=='echo.response')return;
   assert.equal(message.request_id,'c6-smoke');assert.deepEqual(message.data.bytes,new Uint8Array([0,128,255,0]));
   clearTimeout(timeout);socket.close();resolve();
  }catch(error){clearTimeout(timeout);socket.close();reject(error);}
 };
 socket.onerror=()=>{clearTimeout(timeout);socket.close();reject(Error('WS error'));};
});
console.log('PASS native binary CBOR WebSocket echo');
const controller=new AbortController();
const timer=setTimeout(()=>controller.abort(),8000);
try{
 const response=await fetch(new URL('/api/v2/events',base),{signal:controller.signal});assert.equal(response.status,200);
 const reader=response.body.getReader();let text='';
 while(!text.includes('event: diagnostic')||!text.includes('data: ')){const result=await reader.read();assert(!result.done);text+=new TextDecoder().decode(result.value);}
 const line=text.split('\n').find(line=>line.startsWith('data: '));assert.equal(JSON.parse(line.slice(6)).source,'device');
 await reader.cancel();console.log('PASS real device SSE diagnostics');
}finally{clearTimeout(timer);controller.abort();}

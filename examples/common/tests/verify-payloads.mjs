import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {decode} from 'cborg';
const directory=process.argv[2];
for(const target of ['c3','s3','c6'])for(const state of ['connected','unavailable']){
 const bytes=await readFile(`${directory}/${target}-${state}.cbor`),details=decode(bytes);
 assert.deepEqual(Object.keys(details),['hardware','flash_memory','mac_addresses','network','board','build']);
 assert.equal(details.hardware.chip_model,`ESP32-${target.toUpperCase()}`);
 assert.equal(details.hardware.cores,target==='s3'?2:1);
 assert.equal(details.hardware.wifi_6,target==='c6');
 assert.equal(details.mac_addresses.custom_mac,null);
 assert.equal(details.flash_memory.psram_bytes,target==='s3'?8388608:0);
 assert.equal(details.board.rgb_gpio,{c3:2,s3:6,c6:8}[target]);
 assert.equal(details.board.rgb_count,target==='c6'?1:10);
 assert.equal(details.board.rgb_order,target==='c6'?'RGB':'GRB');
 assert.equal(details.build.elf_sha256.length,64);
 if(target==='c6')assert.equal(details.flash_memory.embedded_flash,true);
 if(target==='c3')assert.equal(details.board.rgb_driver,'SPI');
 if(target==='s3'){assert.equal(details.board.lcd_interface,'RGB565');assert.equal(details.board.touch_controller,'GT911');assert.equal(details.board.lcd_ready,true);}
 if(state==='connected'){
  assert.equal(details.network.wifi_mode,'AP+STA');assert.equal(details.network.ssid,'S'.repeat(33));
  assert.equal(details.mac_addresses.ieee802154_eui64,target==='c6'?'01:02:03:04:05:06:07:08':null);
 }else{
  for(const field of ['ipv4','ssid','ap_bssid','channel','rssi_dbm','wifi_auth_mode','wifi_mode'])assert.equal(details.network[field],null);
  assert.equal(details.flash_memory.flash_bytes,null);assert.equal(details.flash_memory.flash_jedec_id,null);
  assert(Object.values(details.mac_addresses).every(value=>value===null));
 }
 console.log(`PASS ${target} ${state}: CBOR types, shared groups, board fields and unavailable values (${bytes.length} B)`);
}

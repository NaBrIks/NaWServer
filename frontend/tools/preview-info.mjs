// Loopback UI emulator. All device details below are synthetic test fixtures.
// HTTP/WS/SSE telemetry comes from the existing S3 fixture on port 8790.
import http from 'node:http';import net from 'node:net';import {readFile} from 'node:fs/promises';
import {encodeWire,decodeWire} from '../src/contracts/cbor.ts';
const upstream='http://127.0.0.1:8790';
const dist=new URL('../../examples/touch_lcd7/frontend/dist/',import.meta.url);
const details={
 hardware:{chip_model:'ESP32-S3 (эмулятор)',architecture:'Xtensa',cpu_mhz:240,crystal_mhz:40,apb_mhz:80,revision_major:0,revision_minor:2,cores:2,wifi_24ghz:true,wifi_6:false,bluetooth_le:true,bluetooth_classic:false,ieee802154:false,usb_serial_jtag:true,hp_sram_bytes:524288,lp_sram_bytes:8192},
 flash_memory:{flash_bytes:16777216,flash_jedec_id:'0xEF4018',embedded_flash:false,heap_internal_bytes:327680,heap_dma_bytes:300000,psram_bytes:8388608,app_partition:'factory',app_partition_bytes:3145728},
 mac_addresses:{factory_mac:'02:00:00:00:00:01',custom_mac:null,base_mac:'02:00:00:00:00:01',wifi_sta_mac:'02:00:00:00:00:01',wifi_ap_mac:'02:00:00:00:00:02',bluetooth_mac:'02:00:00:00:00:03',ethernet_mac:null,ieee802154_eui64:null},
 network:{wifi_connected:true,wifi_mode:'STA',ipv4:'127.0.0.1',netmask:'255.0.0.0',gateway:'127.0.0.1',ssid:'Эмулятор / TEST DATA',ap_bssid:'02:00:00:00:00:04',channel:6,rssi_dbm:-53,wifi_auth_mode:3},
 board:{lcd_controller:'ST7262 (test fixture)',lcd_resolution:'800 x 480',lcd_spi_ready:false,lcd_visual_status:'unverified',backlight_percent:30,rgb_count:10,rgb_gpio:6,rgb_order:'GRB',rgb_driver:'RMT',sd_slot:true,sd_mounted:false,touch:true,ble_started:false},
 build:{project:'S3 UI EMULATOR',version:'TEST DATA',idf_version:'5.4.0',build_date:'Oct 20 2026',build_time:'12:00:00',secure_version:0,elf_sha256:'0'.repeat(64),reset_reason:0}
};
const server=http.createServer(async(request,response)=>{
 try{
  const path=new URL(request.url,'http://127.0.0.1:8792').pathname;
  if(path==='/api/v2/info'){
   const original=await fetch(upstream+path,{signal:AbortSignal.timeout(5000)});
   const info=decodeWire(new Uint8Array(await original.arrayBuffer()));
   info.details=details;info.firmware.board_label='ЭМУЛЯТОР S3 · TEST DATA';
   response.writeHead(200,{'Content-Type':'application/cbor','Cache-Control':'no-store'});response.end(encodeWire(info));return;
  }
  if(path.startsWith('/api/')||path.startsWith('/sim/')){
   const proxy=http.request(upstream+request.url,{method:request.method,headers:request.headers},result=>{response.writeHead(result.statusCode,result.headers);result.pipe(response);});
   proxy.on('error',error=>{if(!response.headersSent)response.writeHead(502);response.end(String(error));});
   request.pipe(proxy);response.on('close',()=>proxy.destroy());return;
  }
  const relative=decodeURIComponent(path==='/'?'index.html':path.slice(1));if(relative.includes('..')){response.writeHead(400);response.end();return;}
  const bytes=await readFile(new URL(relative,dist));const extension=relative.split('.').at(-1);
  response.writeHead(200,{'Content-Type':{html:'text/html',js:'text/javascript',css:'text/css',woff:'font/woff',svg:'image/svg+xml',txt:'text/plain'}[extension]??'application/octet-stream','Cache-Control':'no-store'});response.end(bytes);
 }catch(error){if(!response.headersSent)response.writeHead(500);response.end(String(error));}
});
server.on('upgrade',(request,socket,head)=>{
 const proxy=net.connect(8790,'127.0.0.1',()=>{proxy.write(`${request.method} ${request.url} HTTP/${request.httpVersion}\r\n`+request.rawHeaders.reduce((text,value,index)=>text+value+(index%2?'\r\n':': '),'')+'\r\n');if(head.length)proxy.write(head);socket.pipe(proxy).pipe(socket);});
 proxy.on('error',()=>socket.destroy());socket.on('error',()=>proxy.destroy());socket.on('close',()=>proxy.destroy());
});
server.listen(8792,'127.0.0.1',()=>console.log('S3 UI EMULATOR / TEST DATA: http://127.0.0.1:8792'));

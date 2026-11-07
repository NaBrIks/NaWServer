import {readFileSync,writeFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
const path=process.argv[2];
if(!path)throw Error('Usage: node check-image.mjs compact.ota.uf2');
const file=readFileSync(path),slot=0xff000;
if(!file.length||file.length%512||file.length>slot*2)throw Error('UF2 size exceeds preserved A/B slot');
const count=file.length/512,first=Buffer.alloc(4096,255);
for(let index=0;index<count;index++){
 const block=file.subarray(index*512,(index+1)*512),word=offset=>block.readUInt32LE(offset);
 if(word(0)!==0x0a324655||word(4)!==0x9e5d5157||word(508)!==0x0ab16f30||word(8)!==0x2000||word(28)!==0xe48bff59||word(16)!==256||word(20)!==index||word(24)!==count||word(12)!==0x10000000+index*256)throw Error('Rejected UF2 frame '+index+' (family/address/flags/sequence)');
 if(index<16)block.copy(first,index*256,32,288);
}
const product=Buffer.from('NaWServer-compact-Pico2W');
if(first.indexOf(product)<0)throw Error('Compact board/layout marker absent from first sector');
let tbyb=false;
for(let i=0;i+8<first.length;i+=4)if(first.readUInt32LE(i)===0xffffded3&&first.readUInt32LE(i+4)===0x90210142)tbyb=true;
if(!tbyb)throw Error('Secure Arm RP2350 TBYB boot metadata absent');
const result={file:path,wire_bytes:file.length,payload_bytes:count*256,slot_bytes:slot,sha256:createHash('sha256').update(file).digest('hex'),preserves_fs:'0x200000..0x3bffff',preserves_nvs:'0x3c0000..0x3fffff'};
writeFileSync(path+'.json',JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify(result));

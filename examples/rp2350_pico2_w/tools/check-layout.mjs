import {stat} from 'node:fs/promises';
const file = process.argv[2].replace(/\.elf\.bin$/, '.bin');
const size = (await stat(file)).size;
if(size > 0x200000) throw Error(`Firmware ${size} bytes overlaps FS at 0x200000`);
console.log(`Flash: firmware ${size} / 2097152 bytes; FS 1835008; NVS 262144`);

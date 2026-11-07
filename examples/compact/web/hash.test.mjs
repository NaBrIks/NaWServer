import test from 'node:test';
import assert from 'node:assert/strict';
import {createHash,randomBytes} from 'node:crypto';
import {firmwareHash} from './sha256.mjs';
test('incremental browser SHA-256 matches native padding/chunk boundary vectors',async()=>{
 for(const n of [0,1,55,56,63,64,65,1000,65536,300000]){
  const bytes=randomBytes(n);assert.equal(await firmwareHash(new Blob([bytes])),createHash('sha256').update(bytes).digest('hex'));
 }
});

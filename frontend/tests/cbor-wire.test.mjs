import {test} from 'node:test';
import assert from 'node:assert/strict';
import {encode,decode} from 'cborg';
import {encodeWire,decodeWire} from '../src/contracts/cbor.ts';
test('CBOR contains actual text names and native binary values',()=>{
 const source={v:2,board_label:'S3',new_field:{'температура':23.5,samples:[true,null,new Uint8Array([0,128,255])]},counter:18446744073709551615n};
 const bytes=encodeWire(source);
 const raw=decode(bytes,{useMaps:true});
 assert.equal(raw.get('board_label'),'S3');
 assert(!raw.has(23));
 assert.equal(raw.get('new_field').get('температура'),23.5);
 const result=decodeWire(bytes);
 assert.equal(result.counter,source.counter);
 assert.deepEqual(result.new_field.samples[2],new Uint8Array([0,128,255]));
});
test('unknown valid names survive decode without a registration list',()=>{
 const result=decodeWire(encode(new Map([['future_extension',new Map([['new_nested_key','value']])]])));
 assert.equal(result.future_extension.new_nested_key,'value');
 assert.equal(Object.getPrototypeOf(result),null);
});
test('numeric IDs, duplicates, trailing data and invalid UTF-8 are rejected',()=>{
 const invalid=[
  encode(new Map([[23,'S3']])),
  new Uint8Array([0xa2,0x61,0x76,2,0x61,0x76,2]),
  new Uint8Array([0xa0,0]),
  new Uint8Array([0xbf,0xff]),
  new Uint8Array([0xa1,0x61,0xff,0])
 ];
 for(const bytes of invalid)assert.throws(()=>decodeWire(bytes));
 assert.throws(()=>encodeWire(new Map([[23,'S3']])));
});
test('codec resource and finite value limits remain enforced',()=>{
 assert.throws(()=>decodeWire(new Uint8Array(65537)));
 assert.throws(()=>encodeWire({samples:Array(257).fill(0)}));
 assert.throws(()=>encodeWire({value:NaN}));
 let nested={};for(let i=0;i<14;i++)nested={child:nested};
 assert.throws(()=>encodeWire(nested));
});

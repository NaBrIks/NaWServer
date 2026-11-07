import {encode,decode,Tokenizer,Type} from 'cborg';
const utf8=new TextDecoder('utf-8',{fatal:true,ignoreBOM:true});
class TextTokenizer extends Tokenizer {
 next(){const token=super.next();if(token.type===Type.string&&token.byteValue)token.value=utf8.decode(token.byteValue);return token;}
}
// Wire keys are UTF-8 names. New fields require no registration.
function pack(value:unknown,depth=0):unknown {
 if(depth>12)throw Error('CBOR depth');
 if(typeof value==='number'&&!Number.isFinite(value))throw Error('Nonfinite CBOR value');
 if(value instanceof Uint8Array||value===null||typeof value!=='object')return value;
 if(Array.isArray(value)){if(value.length>256)throw Error('CBOR array limit');return value.map(item=>pack(item,depth+1));}
 const fields=value instanceof Map?[...value]:Object.entries(value).filter(([,item])=>item!==undefined);
 if(fields.length>128)throw Error('CBOR map limit');
 return new Map(fields.map(([name,item])=>{if(typeof name!=='string')throw Error('CBOR text key required');return [name,pack(item,depth+1)];}));
}
function unpack(value:unknown,depth=0):unknown {
 if(depth>12)throw Error('CBOR depth');
 if(value instanceof Map){
  if(value.size>128)throw Error('CBOR map limit');
  const result:Record<string,unknown>=Object.create(null);
  for(const [name,item] of value){if(typeof name!=='string')throw Error('CBOR text key required');result[name]=unpack(item,depth+1);}
  return result;
 }
 if(Array.isArray(value)){if(value.length>256)throw Error('CBOR array limit');return value.map(item=>unpack(item,depth+1));}
 return value;
}
export function encodeWire(value:unknown){return encode(pack(value));}
export function decodeWire(data:Uint8Array){if(data.byteLength>65536)throw Error('CBOR byte limit');return unpack(decode(data,{tokenizer:new TextTokenizer(data,{retainStringBytes:true,allowBigInt:true,strict:true,allowIndefinite:false,allowNaN:false,allowInfinity:false,allowUndefined:false}),useMaps:true,allowIndefinite:false,allowNaN:false,allowInfinity:false,allowUndefined:false,rejectDuplicateMapKeys:true,strict:true}));}
export const diagnostic=(value:unknown)=>JSON.stringify(value,(_,item)=>typeof item==='bigint'?item.toString()+'n':item instanceof Uint8Array?{bytes:item.length,hex:Array.from(item.slice(0,64),byte=>byte.toString(16).padStart(2,'0')).join(' ')}:item,2);
export const cborBody=(value:Record<string,unknown>)=>({headers:{'Content-Type':'application/cbor'},body:encodeWire({v:2,...value}) as BodyInit});
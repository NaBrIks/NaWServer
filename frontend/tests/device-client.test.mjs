import test from 'node:test';
import assert from 'node:assert/strict';
import {DeviceClient,encodeWire} from '../src/client/index.ts';

test('browser client imports without Vue or starting network and preserves CBOR/auth metadata',async()=>{
 let calls=0;
 const client=new DeviceClient({baseUrl:'http://device.test/',fetch:async(url,init)=>{
  calls++;assert.equal(url,'http://device.test/api/v2/info');assert.equal(init.credentials,'omit');
  return new Response(encodeWire({model:'RP2350',bytes:new Uint8Array([1,2])}),{status:200,headers:{'Content-Type':'application/cbor'}});
 }});
 assert.equal(calls,0);
 const response=await client.request('/api/v2/info');assert.equal(calls,1);assert.equal(response.data.model,'RP2350');
 assert.deepEqual(response.data.bytes,new Uint8Array([1,2]));
});

test('HTTP failure remains a response and exposes challenge/retry metadata',async()=>{
 const client=new DeviceClient({fetch:async()=>new Response('busy',{status:503,headers:{'Retry-After':'2','WWW-Authenticate':'Basic realm="test"'}})});
 const response=await client.request('/api');assert.equal(response.ok,false);assert.equal(response.status,503);
 assert.equal(response.retryAfter,'2');assert.equal(response.challenge,'Basic realm="test"');assert.equal(response.text,'busy');
});

test('response limit cancels a streaming body before retaining unbounded data',async()=>{
 let cancelled=false;
 const client=new DeviceClient({maximumResponseBytes:4,fetch:async()=>new Response(new ReadableStream({
  pull(controller){controller.enqueue(new Uint8Array(3));},cancel(){cancelled=true;}
 }))});
 await assert.rejects(client.request('/large'),/Response byte limit/);assert.equal(cancelled,true);
});

test('caller cancellation remains connected to client timeout signal',async()=>{
 const controller=new AbortController();let requestSignal;
 const client=new DeviceClient({fetch:async(_url,init)=>{requestSignal=init.signal;return new Response('ok');}});
 await client.request('/api',{signal:controller.signal});controller.abort();assert.equal(requestSignal.aborted,true);
});

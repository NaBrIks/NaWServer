import test from 'node:test';
import assert from 'node:assert/strict';
import {DeviceClient,pageId,pageHeaders,decodeWire,encodeWire} from '../src/client/index.ts';
import {createPageId} from '../src/client/page_identity.ts';
import {ApiKeyAdmin} from '../src/client/api_keys.ts';

test('document label is shared by clients, fresh labels are random and never persisted',async()=>{
 const seen=[];
 const fetcher=async(_url,init)=>{seen.push(new Headers(init.headers).get('X-Page-Id'));return new Response('ok');};
 await new DeviceClient({fetch:fetcher}).request('/one');await new DeviceClient({fetch:fetcher}).request('/two');
 assert.equal(seen[0],seen[1]);assert.equal(seen[0],pageId());assert.match(seen[0],/^[a-f0-9]{32}$/);
 assert.notEqual(createPageId(),createPageId());
 const headers=pageHeaders({'X-Page-Id':'caller-label',Authorization:'Basic abc'});
 assert.equal(headers.get('X-Page-Id'),pageId());assert.equal(headers.get('Authorization'),'Basic abc');
});

test('WS and SSE carry same non-secret page label while disabled clients add none',async()=>{
 const originalWs=globalThis.WebSocket,originalEvents=globalThis.EventSource;
 globalThis.WebSocket=class{constructor(url){this.url=String(url);}};
 globalThis.EventSource=class{constructor(url){this.url=String(url);}};
 try{
  const client=new DeviceClient({baseUrl:'http://device.test/'});
  const ws=new URL(client.openWebSocket('/ws?x=1').url),sse=new URL(client.openEvents().url);
  assert.equal(ws.protocol,'ws:');assert.equal(ws.searchParams.get('x'),'1');
  assert.equal(ws.searchParams.get('page_id'),pageId());assert.equal(sse.searchParams.get('page_id'),pageId());
  let headers;const disabled=new DeviceClient({baseUrl:'http://device.test/',pageIdentity:false,fetch:async(_url,init)=>{headers=new Headers(init.headers);return new Response('ok');}});
  await disabled.request('/api');assert.equal(headers.has('X-Page-Id'),false);
  assert.equal(new URL(disabled.openWebSocket().url).searchParams.has('page_id'),false);
  assert.equal(new URL(disabled.openEvents().url).searchParams.has('page_id'),false);
 }finally{globalThis.WebSocket=originalWs;globalThis.EventSource=originalEvents;}
});

test('key management carries authenticated bounded metadata and one-time secret without secret URLs',async()=>{
 const requests=[];
 const client=new DeviceClient({fetch:async(url,init)=>{
  requests.push({url,init});assert.equal(url,'/api/v2/keys');assert.ok(new Headers(init.headers).get('Authorization').startsWith('Basic '));
  const data=init.method==='POST'?{slot:0,secret:'one-time-test-secret'}:{maximum_slots:4,slots:[{slot:0,name:'collector',active:true,revision:2}]};
  return new Response(encodeWire(data),{headers:{'Content-Type':'application/cbor'}});
 }}),admin=new ApiKeyAdmin(client);
 const metadata=await admin.list('demo','fixture');assert.equal(metadata.slots[0].name,'collector');assert.equal('secret'in metadata.slots[0],false);
 const created=await admin.change('create',0,'collector','demo','fixture');assert.equal(created.secret,'one-time-test-secret');
 const body=decodeWire(new Uint8Array(requests[1].init.body));assert.equal(body.action,'create');assert.equal(body.slot,0);assert.equal(body.name,'collector');
 assert.equal(JSON.stringify(requests.map(item=>item.url)).includes(created.secret),false);
 const denied=new ApiKeyAdmin(new DeviceClient({fetch:async()=>new Response('denied',{status:401})}));await assert.rejects(denied.list('wrong','wrong'),/401/);
});

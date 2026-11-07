import test from 'node:test';
import assert from 'node:assert/strict';
import {SnapshotPoller,decodeObservers,observerStorageKey} from '../src/client/observers.ts';
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(predicate){const end=Date.now()+1000;while(!predicate()){assert(Date.now()<end,'timed out');await pause(1);}}

test('observer preferences default to WS/SSE only and isolate device origin and profile',()=>{
 assert.deepEqual(decodeObservers(null),{ws:true,http:false,sse:true});
 assert.deepEqual(decodeObservers('{"ws":false,"http":true,"sse":false}'),{ws:false,http:true,sse:false});
 assert.deepEqual(decodeObservers('{"ws":"false","http":1}'),{ws:true,http:false,sse:true});
 assert.deepEqual(decodeObservers('invalid'),{ws:true,http:false,sse:true});
 assert.notEqual(observerStorageKey('demo','http://a/'),observerStorageKey('demo','http://b/'));
 assert.notEqual(observerStorageKey('one','http://a/'),observerStorageKey('two','http://a/'));
 assert.notEqual(observerStorageKey('demo','http://a/'),observerStorageKey('demo','http://a:8080/'));
});

test('explicit HTTP observer aborts in-flight polling and a late result cannot reconnect it',async()=>{
 const calls=[];const releases=[];
 const poller=new SnapshotPoller(async signal=>{calls.push(signal);await new Promise(resolve=>releases.push(resolve));},5);
 await pause(15);assert.equal(calls.length,0);
 poller.setEnabled(true);await until(()=>calls.length===1);
 poller.setEnabled(false);assert.equal(calls[0].aborted,true);
 poller.setEnabled(true);await until(()=>calls.length===2);
 // Re-enable before the canceled generation resolves. Its late finally must
 // not create a second polling loop beside the still-running new request.
 releases.shift()();await pause(20);assert.equal(calls.length,2);
 poller.setEnabled(false);releases.shift()();await pause(20);assert.equal(calls.length,2);
});

test('automatic snapshots never overlap and disabled timers stay canceled',async()=>{
 let active=0,maximum=0,calls=0;
 const poller=new SnapshotPoller(async()=>{calls++;active++;maximum=Math.max(maximum,active);await pause(12);active--;},2);
 poller.setEnabled(true);await until(()=>calls>=3);poller.setEnabled(false);
 await until(()=>active===0);const stopped=calls;await pause(20);
 assert.equal(calls,stopped);assert.equal(maximum,1);
});

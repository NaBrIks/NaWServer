import {readFile} from 'node:fs/promises';
import assert from 'node:assert/strict';
import ts from 'typescript';

// Exercise the production store with a deterministic clock, without browser UI.
let now=0,id=0;const timers=new Map();
const original={performance:globalThis.performance,setInterval:globalThis.setInterval,clearInterval:globalThis.clearInterval};
Object.defineProperty(globalThis,'performance',{configurable:true,value:{now:()=>now}});
globalThis.setInterval=callback=>{timers.set(++id,callback);return id;};
globalThis.clearInterval=key=>timers.delete(key);
const advance=ms=>{now+=ms;for(const tick of [...timers.values()])tick();};
try{
 const source=await readFile(new URL('../src/state/notifications.ts',import.meta.url),'utf8');
 const js=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ESNext}}).outputText.replace(/from ['"]vue['"]/g,`from '${import.meta.resolve('vue')}'`);
 const n=await import('data:text/javascript;base64,'+Buffer.from(js).toString('base64'));
 n.notify('Saved','success');n.notify('Information','info');n.notify('Warning','warning');n.notify('Failed','error');
 advance(3501);assert.deepEqual(n.notices.map(x=>x.kind),['info','warning','error']);
 advance(1500);assert.deepEqual(n.notices.map(x=>x.kind),['warning','error']);
 advance(3000);assert.deepEqual(n.notices.map(x=>x.kind),['error']);
 advance(60000);assert.equal(n.notices.length,1,'error must stay visible');
 n.clearNotices();assert.equal(timers.size,0);
 const first=n.notify('Connection failed','error',{key:'http'});
 assert.equal(n.notify('Connection still failed','error',{key:'http'}),first);assert.equal(n.notices.length,1);
 n.dismissNoticeKey('http');assert.equal(n.notices.length,0);
 const same=n.notify('Same','info');assert.equal(n.notify('Same','info'),same);
 n.noticePause.pointer=true;advance(9000);assert.equal(n.notices[0].remaining,5000);
 n.noticePause.pointer=false;n.noticePause.focus=true;advance(9000);assert.equal(n.notices[0].remaining,5000);
 n.noticePause.focus=false;advance(5001);assert.equal(n.notices.length,0);
 for(let i=0;i<9;i++)n.notify('Event '+i,'info');assert.equal(n.notices.length,5,'stack is bounded');
 n.dismissNotice(n.notices[0].id);assert.equal(n.notices.length,4);
 n.clearNotices();assert.equal(n.notices.length,0);assert.equal(timers.size,0);
 console.log('PASS: severity timeouts, persistent errors, deduplication, pointer/focus pause, bounded stack, individual / keyed / all dismissal, timer cleanup');
}finally{Object.defineProperty(globalThis,'performance',{configurable:true,value:original.performance});globalThis.setInterval=original.setInterval;globalThis.clearInterval=original.clearInterval;}

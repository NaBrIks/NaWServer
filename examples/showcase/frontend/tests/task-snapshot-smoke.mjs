import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import ts from 'typescript';

// Run the actual production poll/tasks functions with controlled HTTP outcomes.
const source=await readFile(new URL('../src/state/device.ts',import.meta.url),'utf8');
const functions=source.slice(source.indexOf('async function tasks()'),source.indexOf('function connect()'));
class HttpError extends Error{constructor(status){super(`HTTP ${status}`);this.status=status;}}
const old={id:'old',cpu_pct:1};
const ui={boot:'boot',info:{boot_id:'boot'},tasks:[old],taskHistory:{old:[{t:1,value:1}]},httpOnline:false,error:''};
let mode='expired',scheduled=0,offsets=[];
const getCbor=async path=>{
 if(path==='/api/v2/summary')return {summary:true};
 offsets.push(path);
 if(mode==='failed')throw new HttpError(500);
 if(path.includes('offset=8')&&mode==='expired')throw new HttpError(409);
 return {boot_id:'boot',captured_ms:2,data:{snapshot_id:'new',next_offset:path.includes('offset=0')?8:null,items:[{id:path.includes('offset=0')?'a':'b',cpu_pct:2}]}};
};
const js=ts.transpileModule(functions,{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ESNext}}).outputText;
const poll=new Function('ui','getCbor','accept','isInfo','isTaskPage','HttpError','window',js+';return poll;')(
 ui,getCbor,()=>{},()=>true,()=>true,HttpError,{setTimeout:(_,delay)=>{assert.equal(delay,1000);scheduled++;}}
);
await poll();
assert.equal(ui.httpOnline,true,'a valid HTTP summary must keep RGB controls enabled after task 409');
assert.equal(ui.error,'');assert.deepEqual(ui.tasks,[old],'never publish a partial task snapshot');
assert.deepEqual(ui.taskHistory,{old:[{t:1,value:1}]});
mode='stable';offsets=[];await poll();
assert.ok(offsets[0].includes('offset=0'),'retry from the first page');
assert.deepEqual(ui.tasks.map(t=>t.id),['a','b']);assert.equal(ui.taskHistory.b.length,1);
mode='failed';await poll();assert.equal(ui.httpOnline,false);assert.equal(ui.error,'HTTP 500','real HTTP failures remain visible');
assert.equal(scheduled,3);
console.log('PASS: task snapshot expiration preserves HTTP/RGB availability and complete data; retry recovers; real failures remain visible');

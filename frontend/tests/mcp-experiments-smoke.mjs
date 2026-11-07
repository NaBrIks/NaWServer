// Explicit live acceptance: starts one short pulse workload and always stops it.
import assert from 'node:assert/strict';
import {decode} from 'cborg';
import {McpClient,MCP_VERSION} from '../http-test/mcp-client.js';
const base=process.argv[2];
if(!base||!/^http:\/\/(127\.0\.0\.1:\d+|192\.168\.\d+\.\d+)$/.test(base))throw Error('Numeric demo URL required');
const authorization='Basic '+btoa('demo:showcase-only');
const headers={Authorization:authorization,Origin:base};
const client=new McpClient((path,init)=>fetch(base+path,{...init,headers:{...init.headers,...headers}}));
const get=async path=>{const response=await fetch(base+path,{signal:AbortSignal.timeout(5000)});assert.equal(response.status,200);return decode(new Uint8Array(await response.arrayBuffer()))};
const state=async()=>{const result=await client.call('experiment_status','{}');assert.equal(result.isError,false);return JSON.parse(result.content[0].text)};
await client.discover();
assert(client.tools.some(tool=>tool.name==='experiment_set'));
const before=await state();assert.equal(before.running,false,'Another experiment is already running; leave it untouched');
for(const Authorization of [undefined,'Basic '+btoa('wrong:wrong')]){
 const response=await fetch(base+'/mcp',{method:'POST',headers:{'Content-Type':'application/json',...(Authorization?{Authorization}:{}),Origin:base},body:'{',signal:AbortSignal.timeout(5000)});
 assert.equal(response.status,401);assert.match(response.headers.get('www-authenticate'),/^Basic /);
}
await assert.rejects(client.call('experiment_set','{"action":"start","duration_ms":999}'),/-32602/);
assert.equal((await state()).running,false);
let started=false;
try{
 const result=await client.call('experiment_set',JSON.stringify({action:'start',kind:'pulse',duration_ms:3000,on_ms:20,period_ms:1000,core:0,priority:1}));
 started=true;assert.equal(result.isError,false);assert.equal(JSON.parse(result.content[0].text).running,true);
 const live=await state();assert.equal(live.running,true);assert.equal(live.kind,'pulse');assert.equal(live.duration_ms,3000);
 assert.equal((await get('/api/v2/experiments')).running,true);
}finally{if(started)await client.call('experiment_set','{"action":"stop"}');}
assert.equal((await state()).running,false);assert.equal((await get('/api/v2/experiments')).running,false);
const info=await get('/api/v2/info');
assert.equal(info.firmware.mcp_enabled,true);
const expectedBackend=process.argv[3]??(info.source==='simulator'?'system_text_json':'nawserver_bounded');
assert.equal(info.firmware.json_backend,expectedBackend);
console.log(JSON.stringify({base,protocol:MCP_VERSION,tools:client.tools.map(tool=>tool.name),unauthorized_rejected:true,invalid_preserves_state:true,start_status_rest_stop:true,json_backend:info.firmware.json_backend,status:'PASS'}));

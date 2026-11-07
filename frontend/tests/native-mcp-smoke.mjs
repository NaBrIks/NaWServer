// Focused live acceptance against a real C++ endpoint (never a runtime bridge).
import assert from 'node:assert/strict';
import {McpClient,MCP_VERSION} from '../http-test/mcp-client.js';
const base=(process.argv[2]||'http://127.0.0.1:8083').replace(/\/$/,'');
const simulator=process.argv.includes('--simulator');
const device=process.argv.includes('--device');
const plainAssets=!simulator&&!device;
const authorization=simulator||device?{Authorization:'Basic '+btoa('demo:showcase-only')}:{};
const client=new McpClient((path,init)=>fetch(base+path,{...init,headers:{...init.headers,...authorization,Origin:base}}));
await client.discover();assert(client.ready);assert(client.tools.some(tool=>tool.name==='device_info'));assert(client.tools.some(tool=>tool.name==='echo'));
const info=await client.call('device_info','{}');assert.equal(info.isError,false);assert.equal(JSON.parse(info.content[0].text).read_only,true);if(simulator)assert.equal(JSON.parse(info.content[0].text).source,'simulator');
if(device)assert.match(JSON.parse(info.content[0].text).target,/^esp32(c6|s3)$/);
const text='Native C++ · Привет!';const echo=await client.call('echo',JSON.stringify({text}));assert.equal(echo.content[0].text,text);assert.equal(echo.resultType,'complete');
await assert.rejects(client.call('echo','{"text":"x","extra":1}'),/JSON-RPC -32602/);
await assert.rejects(client.call('echo',JSON.stringify({text:'я'.repeat(129)})),/JSON-RPC -32602/);
assert.equal((await fetch(base+'/mcp',{headers:authorization})).status,405);
for(const asset of plainAssets?['mcp-client.js','mcp-panel.js']:[]){const response=await fetch(base+'/'+asset);assert.equal(response.status,200);assert.match(response.headers.get('content-type'),/javascript/);assert((await response.text()).length>50)}
const request={jsonrpc:'2.0',id:99,method:'server/discover',params:{_meta:{'io.modelcontextprotocol/protocolVersion':MCP_VERSION,'io.modelcontextprotocol/clientCapabilities':{},'io.modelcontextprotocol/clientInfo':{name:'native-acceptance',version:'1'}}}};
for(const origin of ['http://evil.invalid:8083','http://127.0.0.1:8084']){const response=await fetch(base+'/mcp',{method:'POST',headers:{'Content-Type':'application/json',Accept:'application/json, text/event-stream','MCP-Protocol-Version':MCP_VERSION,'Mcp-Method':'server/discover',Origin:origin,...authorization},body:JSON.stringify(request)});assert.equal(response.status,403)}
console.log(JSON.stringify({source:base,protocol:MCP_VERSION,tools:client.tools.map(tool=>tool.name),discovery:true,device_info:true,utf8_echo:true,invalid_arguments_rejected:true,utf8_byte_limit:true,assets:plainAssets?2:0,hostile_origins_rejected:2,get_status:405}));

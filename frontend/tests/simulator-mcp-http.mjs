import assert from 'node:assert/strict';
const base=process.argv[2]||'http://127.0.0.1:8798';
const version='2026-07-28';
const meta={'io.modelcontextprotocol/protocolVersion':version,'io.modelcontextprotocol/clientCapabilities':{}};
const source={jsonrpc:'2.0',id:1,method:'server/discover',params:{_meta:meta}};
const headers={'Content-Type':'application/json',Accept:'application/json, text/event-stream','MCP-Protocol-Version':version,'Mcp-Method':'server/discover',Origin:base};
async function probe(body,expected,code,changes={}){const response=await fetch(base+'/mcp',{method:'POST',headers:{...headers,...changes},body:typeof body==='string'?body:JSON.stringify(body)});assert.equal(response.status,expected);const raw=await response.text();assert(Buffer.byteLength(raw)<=4096);if(code)assert.equal(JSON.parse(raw).error.code,code)}
await probe(source,415,null,{'Content-Type':'application/cbor'});
await probe(source,406,null,{Accept:'text/event-stream'});
await probe(source,406,null,{Accept:'application/json;q=0, text/event-stream'});
await probe(source,400,-32020,{'Mcp-Method':'ping'});
await probe({...source,params:{}},400,-32602);
await probe({...source,params:{_meta:{...meta,'io.modelcontextprotocol/protocolVersion':'old'}}},400,-32022,{'MCP-Protocol-Version':'old'});
await probe({...source,method:'unknown'},404,-32601,{'Mcp-Method':'unknown'});
await probe('{',200,-32700);
await probe(' '.repeat(4097),413);
const index=await(await fetch(base+'/')).text();const script=index.match(/src="([^"]+\.js)"/)[1];assert.equal((await fetch(base+script)).status,200);
for(const path of ['/assets/%2e%2e/Showcase.Host.csproj','/assets/..%5cShowcase.Host.csproj','/assets/%2fetc/passwd','/assets/'])assert.equal((await fetch(base+path)).status,404);
console.log('Simulator MCP HTTP/asset bounds: 14 checks PASS');

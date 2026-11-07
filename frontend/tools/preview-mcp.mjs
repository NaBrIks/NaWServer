// Isolated browser QA fixture only. Never used by deployed MCP clients.
import http from 'node:http';
import {readFile} from 'node:fs/promises';
const port=Number(process.env.MCP_PREVIEW_PORT||8797);
const tools=[{name:'device_info',description:'Read-only TEST FIXTURE identity',inputSchema:{type:'object',additionalProperties:false},annotations:{readOnlyHint:true}},{name:'echo',description:'Echo UTF-8 text (TEST FIXTURE)',inputSchema:{type:'object',properties:{text:{type:'string',maxLength:256}},required:['text'],additionalProperties:false},annotations:{readOnlyHint:true}}];
http.createServer(async(request,response)=>{try{const url=new URL(request.url,'http://127.0.0.1');
 if(url.pathname==='/mcp'){
  if(new URL(request.headers.referer||'http://127.0.0.1').searchParams.has('mcp-disabled')){response.writeHead(404).end();return}
  let text='';for await(const chunk of request){text+=chunk;if(text.length>4096){response.writeHead(413).end();return}}
  const value=JSON.parse(text);if(value.method==='notifications/initialized'){response.writeHead(202).end();return}
  const result=value.method==='server/discover'?{supportedVersions:['2026-07-28'],_meta:{'io.modelcontextprotocol/serverInfo':{name:'MCP BROWSER TEST FIXTURE',version:'1'}},capabilities:{tools:{listChanged:false}}}:value.method==='tools/list'?{tools}:{content:[{type:'text',text:value.params?.name==='echo'?value.params.arguments.text:'TEST FIXTURE only; no device attached'}],isError:false};
  response.writeHead(200,{'Content-Type':'application/json'}).end(JSON.stringify({jsonrpc:'2.0',id:value.id,result:{...result,resultType:'complete'}}));return;
 }
 if(url.pathname.startsWith('/api/')||url.pathname==='/sim/state'){response.writeHead(404).end();return}
 const plain=url.pathname==='/http-test/'||['/http-test.css','/http-test.js','/mcp-client.js','/mcp-panel.js','/http-test-config.json'].includes(url.pathname);
 let relative=plain?(url.pathname==='/http-test/'?'index.html':url.pathname==='/http-test-config.json'?'config.windows.json':url.pathname.slice(1)):url.pathname==='/'?'index.html':url.pathname.slice(1);
 if(relative.includes('..')){response.writeHead(400).end();return}
 const path=new URL(plain?'../http-test/'+relative:'../../examples/showcase/frontend/dist/'+relative,import.meta.url);
 const bytes=await readFile(path);response.writeHead(200,{'Content-Type':({html:'text/html',js:'text/javascript',css:'text/css',json:'application/json',woff:'font/woff'})[relative.split('.').at(-1)]||'application/octet-stream'}).end(bytes);
 }catch(error){response.writeHead(404).end(String(error))}
}).listen(port,'127.0.0.1',()=>console.log(`MCP UI TEST FIXTURE http://127.0.0.1:${port}`));

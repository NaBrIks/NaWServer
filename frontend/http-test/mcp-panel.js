import {McpClient} from './mcp-client.js';
const client=new McpClient();
const element=id=>document.getElementById(id);
const initialize=element('mcp-initialize'),call=element('mcp-call'),select=element('mcp-tool');
function choose(){element('mcp-arguments').value=select.value==='echo'?'{"text":"Привет, MCP!"}':'{}';element('mcp-schema').textContent=JSON.stringify(client.tools.find(tool=>tool.name===select.value)?.inputSchema??{},null,2)}
async function perform(connect){initialize.disabled=true;call.disabled=true;element('mcp-result').textContent='—';try{if(connect){await client.discover();select.replaceChildren(...client.tools.map(tool=>{const option=document.createElement('option');option.value=tool.name;option.textContent=tool.name;option.title=tool.description||tool.name;return option}));choose();element('mcp-capabilities').textContent=JSON.stringify(client.server);element('mcp-status').textContent='Подключён'}else{const result=await client.call(select.value,element('mcp-arguments').value);element('mcp-result').textContent=client.lastResponseText;element('mcp-status').textContent=result.isError?'Ошибка инструмента':'Выполнено'}}catch(error){element('mcp-status').textContent=String(error)}finally{initialize.disabled=false;call.disabled=!client.ready||!select.value;select.disabled=!client.ready;element('mcp-protocol').textContent=JSON.stringify(client.transcript,null,2)}}
initialize.addEventListener('click',()=>perform(true));call.addEventListener('click',()=>perform(false));select.addEventListener('change',choose);



initialize.disabled=false;

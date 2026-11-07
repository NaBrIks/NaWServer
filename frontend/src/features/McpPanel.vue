<script setup lang="ts">
import {computed,ref,shallowReactive} from 'vue';
import {pageHeaders} from '../client/index';
import {McpClient} from '../../http-test/mcp-client.js';
import {t} from '../state/appearance';
import {ui} from '../state/device';
import {commandCredentials} from '../state/access';
import {jsonBackendName} from '../contracts';
import Icon from '../components/Icon.vue';
const simulated=computed(()=>ui.info?.source==='simulator');
// The existing session credentials protect all MCP operations just like demo commands.
const client=shallowReactive(new McpClient((input,init)=>{
 const headers=pageHeaders(init?.headers);
 headers.set('Authorization','Basic '+btoa(commandCredentials.user+':'+commandCredentials.password));
 return fetch(input,{...init,headers});
}));
const busy=ref(false),status=ref(''),selected=ref(''),argumentsText=ref('{}'),result=ref(''),revision=ref(0);
const tool=computed(()=>client.tools.find(item=>item.name===selected.value));
const raw=computed(()=>{revision.value;return JSON.stringify(client.transcript,null,2)});
const content=computed(()=>{if(!result.value)return [];try{const data=JSON.parse(result.value);return (data.result?.content??[]).filter((part:any)=>part.type==='text').map((part:any)=>part.text)}catch{return []}});
function choose(name:string){selected.value=name;const schema=client.tools.find(item=>item.name===name)?.inputSchema as any;const args:Record<string,unknown>={};for(const [key,value] of Object.entries(schema?.properties??{})){const field=value as any;if(field.default!==undefined)args[key]=field.default;else if(schema.required?.includes(key))args[key]=field.enum?.[0]??(field.type==='integer'||field.type==='number'?field.minimum??0:field.type==='boolean'?false:'');}if(name==='echo')args.text='Привет, MCP!';argumentsText.value=JSON.stringify(args,null,2);result.value='';}
function openAccess(){ui.page='access';history.replaceState(null,'','#access');}
async function execute(discover=false){busy.value=true;status.value='';result.value='';client.lastResponseText='';try{if(discover){await client.discover();choose(client.tools.some(item=>item.name===selected.value)?selected.value:client.tools[0]?.name??'');status.value=t('Список получен','Tools received')}else{const value=await client.call(selected.value,argumentsText.value);result.value=client.lastResponseText;status.value=value.isError?t('Ошибка инструмента','Tool error'):t('Выполнено','Completed')}}catch(error){status.value=String(error);result.value=client.lastResponseText}finally{busy.value=false;revision.value++}}
</script>
<template>
<section class="panel mcp-panel" data-testid="mcp-panel">
 <div class="panel-head mcp-heading"><h2><Icon name="mcp"/>{{t('Инструменты MCP','MCP tools')}}</h2><button class="mcp-discover" :disabled="busy" @click="execute(true)">{{t('Получить список инструментов','Get tool list')}}</button></div>
 <dl class="mcp-connection">
  <div><dt>{{t('Сервер','Server')}}</dt><dd>{{simulated?t('Модель симулятора','Simulator model'):t('Устройство','Device')}} · <code>POST /mcp</code></dd></div>
  <div><dt>{{t('JSON-библиотека','JSON library')}}</dt><dd>{{jsonBackendName(ui.info?.firmware.json_backend)}}</dd></div>
  <div><dt>{{t('Учётная запись','Account')}}</dt><dd><b>{{commandCredentials.user}}</b><button class="text-button" @click="openAccess">{{t('Изменить → Доступ','Change → Access')}}</button></dd></div>
 </dl>
 <p class="mcp-status" role="status" data-testid="mcp-status">{{status||t('Нажмите «Получить список инструментов», затем выберите инструмент в таблице.','Click “Get tool list”, then select a tool in the table.')}}</p>
 <h3 class="mcp-step">{{t('1. Список инструментов','1. Tool list')}}</h3>
 <div class="table-wrap"><table class="mcp-tools"><thead><tr><th>{{t('Инструмент','Tool')}}</th><th>{{t('Описание','Description')}}</th></tr></thead><tbody><tr v-for="item in client.tools" :key="item.name" :class="{selected:item.name===selected}"><td><button class="text-button mono" :disabled="busy" :aria-pressed="item.name===selected" @click="choose(item.name)">{{item.name}}</button></td><td :title="item.description">{{item.description||'—'}}</td></tr><tr v-if="!client.tools.length"><td colspan="2">{{t('Список ещё не получен','Tool list not loaded')}}</td></tr></tbody></table></div>
 <form class="mcp-workbench" @submit.prevent="execute()">
  <div><label class="label" for="mcp-tool">{{t('Выбранный инструмент','Selected tool')}}</label><select id="mcp-tool" :value="selected" :disabled="busy||!client.ready" @change="choose(($event.target as HTMLSelectElement).value)"><option v-if="!client.tools.length" value="">—</option><option v-for="item in client.tools" :key="item.name" :value="item.name">{{item.name}}</option></select><p class="endpoint">{{tool?.description}}</p><details v-if="tool"><summary>{{t('Схема аргументов','Argument schema')}} · JSON Schema</summary><pre>{{JSON.stringify(tool.inputSchema,null,2)}}</pre></details></div>
  <div><label class="label mcp-step" for="mcp-arguments">{{t('2. Аргументы JSON','2. JSON arguments')}}</label><textarea id="mcp-arguments" v-model="argumentsText" rows="8" maxlength="4096" :disabled="busy||!client.ready" spellcheck="false"></textarea><button class="primary" :disabled="busy||!client.ready||!selected">{{t('Выполнить инструмент','Execute tool')}}</button></div>
  <div><div class="result-head mcp-step">{{t('3. Полученные данные','3. Returned data')}}</div><pre v-for="(text,index) in content" :key="index" class="terminal mcp-result" data-testid="mcp-data">{{text}}</pre><pre v-if="!content.length" class="terminal mcp-result">—</pre></div>
 </form>
 <details><summary>{{t('Полный ответ JSON-RPC','Full JSON-RPC response')}}</summary><pre class="terminal" data-testid="mcp-result">{{result||'—'}}</pre></details>
 <details><summary>{{t('Журнал · последние 12 обменов','Transcript · last 12 exchanges')}}</summary><pre class="terminal">{{raw}}</pre></details>
 <p class="endpoint">{{t('experiment_status — состояние; experiment_set — запуск и остановка. Параметры проверяет сервер. Учётные данные передаются в HTTP-заголовке, в журнал не записываются.','experiment_status reads state; experiment_set starts and stops a workload. The server validates parameters. Credentials are sent in an HTTP header and excluded from the transcript.')}}</p>
</section>
</template>

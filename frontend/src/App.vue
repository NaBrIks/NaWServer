<script setup lang="ts">
import MemoryStrip from './components/MemoryStrip.vue';
import {memoryRegions} from './contracts/memory';
import {computed,onMounted,defineAsyncComponent,watch} from 'vue';
import {ui,start,stream,formatBytes,clock,summaryVisible,toggleSocket,toggleHttp,toggleEvents,refreshSnapshot} from './state/device';
import {appearance,t} from './state/appearance';
import Icon from './components/Icon.vue';import InfoButton from './components/InfoButton.vue';
import {project} from '@project';
import {defaultPlatform} from './project';
const platform={...defaultPlatform,...project.platform};
import AppearanceMenu from './components/AppearanceMenu.vue';
import ResourcesPanel from './features/ResourcesPanel.vue';import LabPanel from './features/LabPanel.vue';import AccessPanel from './features/AccessPanel.vue';import ExperimentsPanel from './features/ExperimentsPanel.vue';import TransportDiagram from './components/TransportDiagram.vue';
import McpPanel from './features/McpPanel.vue';
import TasksDock from './components/TasksDock.vue';import MiniTrend from './components/MiniTrend.vue';import NotificationStack from './components/NotificationStack.vue';import {notify,dismissNoticeKey} from './state/notifications';
const SimulatorPanel=defineAsyncComponent(()=>import('./features/SimulatorPanel.vue'));
const pages=computed(()=>[
 {id:'overview',icon:'list-details',name:'INFO'},
 {id:'network',icon:'plug',name:t('Соединения','Connections')},
 ...project.pages.map(page=>({id:page.id,icon:page.icon,name:t(page.label.ru,page.label.en)})),
 {id:'mcp',icon:'mcp',name:'MCP'},{id:'lab',icon:'flask',name:t('Лаборатория','Laboratory')},{id:'experiments',icon:'bolt',name:t('Эксперименты','Experiments')},
 {id:'events',icon:'broadcast',name:t('События / SSE','Events / SSE')},{id:'access',icon:'lock',name:t('Доступ','Access')},{id:'settings',icon:'settings',name:t('Настройки','Settings')}]);
const navigationGroups=computed(()=>[
 {id:'system',name:t('Система','System'),pages:['overview']},
 {id:'server',name:t('Веб-сервер','Web server'),pages:['network','mcp','lab','events','access']},
 {id:'demo',name:t('Демка','Demo'),pages:[...project.pages.map(page=>page.id),'experiments']}
].map(group=>({...group,items:group.pages.map(id=>pages.value.find(page=>page.id===id)!)})));
const extensionPage=computed(()=>project.pages.find(page=>page.id===ui.page));
const current=computed(()=>pages.value.find(p=>p.id===ui.page)??pages.value[0]);
const httpFresh=computed(()=>ui.httpOnline),fresh=summaryVisible,s=computed(()=>ui.summary),internal=computed(()=>s.value?.memory.find(p=>p.id==='internal'));
const sseFresh=computed(()=>stream.online&&ui.now-stream.lastSeen<6000);
const exchangeMode=computed(()=>!ui.summary?t('Нет данных','No data'):!ui.wsWanted&&!ui.httpWanted?t('Снимок','Snapshot'):!fresh.value?t('Нет свежих','Stale'):ui.wsWanted&&ui.httpWanted?'WS + HTTP':ui.wsWanted?'WS LIVE':'HTTP LIVE');
const exchangeHint=computed(()=>t('INFO: WS — живые сводки; HTTP 1 Hz — сводка и все страницы задач каждую секунду. При выключенных WS и HTTP значения заморожены до ручного обновления. SSE — отдельный диагностический поток.','INFO: WS supplies live summaries; HTTP 1 Hz requests summary and all task pages every second. With WS and HTTP off, values are frozen until manual refresh. SSE is an independent diagnostic stream.'));

const source=computed(()=>!ui.info?t('Источник не определён','Source unknown'):ui.info.source==='device'?platform.deviceLabel+' · '+t('устройство','device'):t('Синтетическая модель','Synthetic model'));
let httpOutage=false;
watch([httpFresh,()=>ui.error],([online,error])=>{
 if(!online&&error&&!httpOutage){httpOutage=true;notify(t('Ошибка HTTP-снимка','HTTP snapshot failed')+' · '+error,'error',{key:'http-offline'});}
 else if(online&&httpOutage){httpOutage=false;dismissNoticeKey('http-offline');notify(t('HTTP-снимок получен','HTTP snapshot received'),'success');}
});
function navigate(id:string){ui.page=id;history.replaceState(null,'','#'+id);}
onMounted(()=>{let id=location.hash.slice(1);if(['tasks','memory'].includes(id))id='overview';if(pages.value.some(p=>p.id===id))navigate(id);start();});
</script>
<template>
<div class="app-shell" :class="{collapsed:appearance.collapsed,'mcp-page':ui.page==='mcp'}">
 <header class="topbar">
  <button class="icon-button" :aria-label="t('Свернуть меню','Toggle menu')" :data-tooltip="t('Свернуть меню','Toggle menu')" @click="appearance.collapsed=!appearance.collapsed"><Icon name="menu-2"/></button>
  <strong class="brand">NaWServer <small>{{project.label}}</small></strong>
  <div class="top-status"><span class="pill" data-testid="source-mode">{{source}}</span><span class="observer-mode" data-testid="info-exchange" :class="fresh?'ok':''" :data-tooltip="exchangeHint">{{exchangeMode}}</span><button class="observer-button" data-testid="ws-toggle" :aria-pressed="ui.wsWanted" :class="ui.wsWanted?(ui.wsOnline?'ok':'bad'):''" :data-tooltip="t('Живые сводки INFO по WebSocket','Live INFO summaries over WebSocket')" @click="toggleSocket"><span>WS INFO</span><b>{{ui.wsWanted?(ui.wsOnline?'LIVE':'…'):'OFF'}}</b></button><button class="observer-button" data-testid="http-toggle" :aria-pressed="ui.httpWanted" :class="ui.httpWanted?(httpFresh?'ok':'bad'):''" :data-tooltip="t('HTTP: сводка и страницы задач каждую секунду; по умолчанию выключено','HTTP summary and task pages every second; disabled by default')" @click="toggleHttp"><span>HTTP 1 Hz</span><b data-testid="http-state">{{ui.httpWanted?(httpFresh?'LIVE':'…'):'OFF'}}</b></button><button class="observer-button" data-testid="sse-toggle" :aria-pressed="stream.wanted" :class="stream.wanted?(sseFresh?'ok':'bad'):''" :data-tooltip="t('Независимый поток SSE','Independent SSE stream')" @click="toggleEvents"><span>SSE</span><b data-testid="sse-state">{{stream.wanted?(sseFresh?'LIVE':'…'):'OFF'}}</b></button><button class="observer-refresh" data-testid="http-refresh" :disabled="ui.refreshing" :aria-label="t('Обновить HTTP-снимок','Refresh HTTP snapshot')" :data-tooltip="t('Один HTTP-снимок: информация, сводка и задачи','One HTTP snapshot: info, summary and tasks')" @click="refreshSnapshot()"><Icon name="refresh"/></button></div>
  <div class="appearance-controls"><AppearanceMenu/><button class="language-button" aria-label="RU / EN" @click="appearance.language=appearance.language==='ru'?'en':'ru'"><Icon name="language"/>{{appearance.language.toUpperCase()}}</button></div>
 </header>
 <aside class="sidebar"><nav :aria-label="t('Разделы','Navigation')"><section v-for="group in navigationGroups" :key="group.id" class="nav-group" :aria-label="group.name"><h2 class="nav-group-label">{{group.name}}</h2><button v-for="p in group.items" :key="p.id" :class="{active:ui.page===p.id}" :data-tooltip="group.name+' · '+p.name" :aria-label="p.name" :aria-current="ui.page===p.id?'page':undefined" @click="navigate(p.id)"><Icon :name="p.icon"/><span>{{p.name}}</span></button></section><button class="settings" :class="{active:ui.page==='settings'}" :data-tooltip="t('Настройки','Settings')" :aria-label="t('Настройки','Settings')" :aria-current="ui.page==='settings'?'page':undefined" @click="navigate('settings')"><Icon name="settings"/><span>{{t('Настройки','Settings')}}</span></button></nav><div class="sidebar-info"><span class="chip-art">{{ui.info?.chip.model??platform.chipFallback}}</span><small>{{ui.info?.firmware.version??'—'}}</small><span class="status" :class="fresh?'ok':'bad'">{{exchangeMode}}</span></div></aside>
 <div class="workspace">
  <div class="metrics" data-testid="global-metrics" :class="{stale:!fresh}"><span><Icon name="bolt"/>CPU <b data-testid="cpu-value">{{fresh&&s?s.cpu_busy_pct.toFixed(1):'—'}}%</b><MiniTrend :points="ui.cpu" :max="100" :stale="!fresh" :label="t('CPU · последние 60 секунд','CPU · last 60 seconds')"/></span><span><Icon name="database"/>RAM <b data-testid="heap-value">{{fresh&&internal?formatBytes(internal.free_bytes):'—'}}</b><MemoryStrip :regions="memoryRegions(s)" :fresh="fresh"/></span><span><Icon name="clock"/>UP <b>{{fresh&&s?clock(s.uptime_ms):'—'}}</b></span><span><Icon name="list"/>{{t('Задачи','Tasks')}} <b>{{fresh?s?.task_count:'—'}}</b></span><span :data-tooltip="t('Активные WS-клиенты сервера','Active server WS clients')">WS <b>{{s?.ws_clients??0}}</b></span><span :data-tooltip="t('SSE-события, принятые этим браузером','SSE events received by this browser')">SSE RX <b data-testid="sse-count">{{stream.count}}</b> {{t('событий','events')}}</span><span>RSSI <b>{{s?.rssi_dbm??'—'}} dBm</b></span></div>
  <div class="page-strip"><h1>{{current?.name}}</h1><span class="source-banner" :data-tooltip="ui.info?.firmware.board_label??source">{{!ui.info?t('Подключение…','Connecting…'):ui.info.source==='device'?ui.info.firmware.board_label:t('Локальный эмулятор · системные данные синтетические','Local emulator · synthetic system data')}}</span><InfoButton :label="t('Источник и каналы','Source and channels')" help-key="source"/></div>
  <div class="content-grid" :class="{device:!ui.info?.capabilities.simulator_control?.supported||ui.page==='settings'}">
   <main class="content-column" tabindex="0" :aria-label="current?.name">
    <ResourcesPanel v-if="ui.page==='overview'"/>
    <div v-else-if="ui.page==='network'" class="network-panels"><TransportDiagram/><section class="panel transport-counters"><div class="panel-head"><h2>{{t('Транспортные счётчики','Transport counters')}}</h2><InfoButton :label="t('Счётчики','Counters')" help-key="counters"/></div><dl class="details"><div v-for="(value,key) in s?.counters" :key="key"><dt>{{key}}</dt><dd>{{value}}</dd></div><div><dt>SSE / {{t('принято браузером','received by browser')}}</dt><dd>{{stream.count}}</dd></div></dl></section></div>
    <component v-else-if="extensionPage" :is="extensionPage.component"/>
    <LabPanel v-else-if="ui.page==='lab'"/>
    <McpPanel v-else-if="ui.page==='mcp'"/>
    <AccessPanel v-else-if="ui.page==='access'"/>
    <ExperimentsPanel v-else-if="ui.page==='experiments'"/>
    <template v-else-if="ui.page==='events'"><section class="panel"><div class="panel-head"><h2>{{t('Диагностический поток','Diagnostic stream')}} / SSE</h2><span class="pill">JSON · 1 Hz</span></div><div class="stream-toolbar"><span :class="sseFresh?'ok':'bad'">{{sseFresh?'LIVE':'OFFLINE'}}</span><code>/api/v2/events</code><span>JSON · 1 Hz · {{stream.log.length}} / 80</span><span class="mono">{{stream.id||'—'}}</span></div><div class="table-wrap event-timeline"><table><thead><tr><th>ID</th><th class="numeric">{{t('Время','Time')}}</th><th class="numeric">CPU</th><th class="numeric">RAM</th><th class="numeric">{{t('Задачи','Tasks')}}</th></tr></thead><tbody><tr v-for="e in stream.log" :key="e.id"><td>{{e.data.boot_id}}:{{e.data.seq}}</td><td class="numeric">{{clock(e.data.captured_ms)}}</td><td class="numeric">{{e.data.cpu_busy_pct.toFixed(1)}}%</td><td class="numeric">{{formatBytes(e.data.free_bytes)}}</td><td class="numeric">{{e.data.task_count}}</td></tr></tbody></table></div></section><section v-if="ui.events.length" class="panel"><div class="panel-head"><h2>{{t('Журнал модели','Model events')}}</h2></div><div v-for="e in ui.events.slice().reverse()" :key="e.id" class="event-row"><code>{{clock(e.captured_ms)}}</code><b>{{e.category}}</b><span>{{e.message}}</span></div></section></template>
    <section v-else-if="ui.page==='settings'" class="panel"><div class="panel-head"><h2>{{t('Настройки интерфейса','Interface settings')}}</h2></div><p>{{t('Тема, плотность, язык и состояние меню сохраняются только в этом браузере.','Theme, density, language and menu state are saved only in this browser.')}}</p><dl class="details"><div><dt>{{t('Шаблон','Template')}}</dt><dd>UI.TechnicalShell</dd></div><div><dt>{{t('Иконки','Icons')}}</dt><dd>Tabler Icons · local · MIT</dd></div><div><dt>{{t('Тема / плотность','Theme / density')}}</dt><dd>{{appearance.theme}} / {{appearance.density}}</dd></div></dl></section>
    <details v-if="ui.info?.capabilities.simulator_control?.supported&&ui.page!=='settings'" class="simulator-tools"><summary><Icon name="adjustments"/>{{t('Пульт имитации','Simulator controls')}}</summary><SimulatorPanel/></details>
   </main>

  </div>
  <footer><span>{{!ui.info?'CONNECTING':ui.info.source==='device'?ui.info.chip.model+' / '+platform.sdkLabel:'SIMULATOR'}}</span><span>HTTP + WS/CBOR + SSE/JSON</span><span>{{!ui.info?t('Ожидание данных','Waiting for data'):ui.info.source==='device'?t('Данные платы','Device data'):t('Данные модели','Model data')}}</span><NotificationStack/></footer>
 </div><TasksDock/>
</div>
</template>

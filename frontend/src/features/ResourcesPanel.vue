<script setup lang="ts">
import {computed} from 'vue';import {ui,summaryVisible} from '../state/device';import {t} from '../state/appearance';
import TrendChart from '../components/TrendChart.vue';import CpuPanel from '../components/CpuPanel.vue';
import {project} from '@project';import {defaultPlatform} from '../project';
const platform={...defaultPlatform,...project.platform};import Icon from '../components/Icon.vue';import InfoButton from '../components/InfoButton.vue';
import DeviceDetails from './DeviceDetails.vue';import TransmissionPanel from '../components/TransmissionPanel.vue';
import MemoryUsagePanel from '../components/MemoryUsagePanel.vue';
import {memoryRegions} from '../contracts/memory';
import {httpBackendName,jsonBackendName} from '../contracts';
const summary=computed(()=>ui.summary),fresh=summaryVisible;
const internal=computed(()=>summary.value?.memory.find(p=>p.id==='internal'));
const kib=(bytes:number)=>(bytes/1024).toLocaleString(t('ru-RU','en-US'),{minimumFractionDigits:1,maximumFractionDigits:1});
const integer=(value:number)=>value.toLocaleString(t('ru-RU','en-US'));
const budget=(bytes:number|undefined)=>bytes===undefined?'—':kib(bytes)+' '+t('КиБ','KiB');
</script>
<template>
 <div class="two-panels resource-charts" data-testid="resource-charts">
  <CpuPanel :total="summary?.cpu_busy_pct??null" :cores="summary?.core_busy_pct??[]" :total-history="ui.cpu" :core-history="ui.coreCpu" :sample-interval="summary?.sample_interval_ms??1000" :fresh="fresh"/>
  <section class="panel"><div class="panel-head"><h2><Icon name="database"/>{{t('Свободная RAM','Free RAM')}}</h2><span>0–{{kib(internal?.total_bytes??327680)}} {{t('КиБ','KiB')}}</span></div><TrendChart :points="ui.memory" :max="(internal?.total_bytes??327680)/1024" unit=" KiB" color="var(--lu-info)" :label="t('Свободная RAM · от нуля до полного объёма','Free RAM · fixed zero to pool capacity scale')"/></section>
  <TransmissionPanel v-if="summary?.transmission" :stats="summary.transmission" :fresh="fresh" :job-history="ui.txJobs" :memory-history="ui.txMemory" :rate-history="ui.txRate"/>
  <MemoryUsagePanel :regions="memoryRegions(summary)" :fresh="fresh"/>
 </div>
 <div class="resource-summary-panels"><section class="panel" data-testid="memory-pools"><div class="panel-head"><h2><Icon name="database"/>{{t('Пулы памяти','Memory pools')}}</h2><div><span>{{t('КиБ','KiB')}}</span><InfoButton :label="t('Память','Memory')" help-key="memory"/></div></div>
  <div class="table-wrap"><table class="memory-table"><colgroup><col/><col v-for="n in 4" :key="n" class="memory-value-col"/></colgroup><thead><tr><th>{{t('Регион','Region')}}</th><th class="numeric">{{t('Всего','Total')}}</th><th class="numeric" :data-tooltip="t('Свободная память','Free memory')">{{t('Своб.','Free')}}</th><th class="numeric" :data-tooltip="t('Минимум свободной памяти с запуска','Minimum free memory since boot')">{{t('Мин.','Min.')}}</th><th class="numeric" :data-tooltip="t('Крупнейший свободный блок','Largest free block')">{{t('Блок','Block')}}</th></tr></thead><tbody><tr v-for="pool in summary?.memory??[]" :key="pool.id"><td>{{pool.id==='internal'?platform.internalMemoryLabel?t(platform.internalMemoryLabel.ru,platform.internalMemoryLabel.en):t('Внутренняя RAM','Internal RAM'):pool.label}}</td><td class="numeric" :data-tooltip="integer(pool.total_bytes)+' B'">{{kib(pool.total_bytes)}}</td><td class="numeric" :data-tooltip="integer(pool.free_bytes)+' B'">{{fresh?kib(pool.free_bytes):'—'}}</td><td class="numeric" :data-tooltip="integer(pool.min_free_bytes)+' B'">{{kib(pool.min_free_bytes)}}</td><td class="numeric" :data-tooltip="integer(pool.largest_block_bytes)+' B'">{{fresh?kib(pool.largest_block_bytes):'—'}}</td></tr></tbody></table></div>
 </section>

 <section v-if="!ui.info?.details" class="panel resource-identity"><div class="panel-head"><h2>{{t('Паспорт устройства','Device identity')}}</h2><span>{{ui.info?.chip.psram?'PSRAM':t('Без PSRAM','No PSRAM')}}</span></div><dl class="details"><div><dt>{{t('Чип / ядра','Chip / cores')}}</dt><dd>{{ui.info?.chip.model??'—'}} / {{ui.info?.chip.cores??'—'}}</dd></div><div><dt>{{platform.sdkLabel}}</dt><dd>{{ui.info?.firmware.idf_version??'—'}}</dd></div><div><dt>HTTP / WS / SSE</dt><dd>{{httpBackendName(ui.info?.firmware.http_backend)}}</dd></div><div><dt>{{t('JSON-библиотека MCP','MCP JSON library')}}</dt><dd>{{jsonBackendName(ui.info?.firmware.json_backend)}}</dd></div><div><dt>MCP</dt><dd>{{ui.info?.firmware.mcp_enabled===undefined?'—':ui.info.firmware.mcp_enabled?t('Включён','Enabled'):t('Выключен','Disabled')}}</dd></div><div><dt>{{t('JSON: рабочая область','JSON: workspace')}}</dt><dd>{{budget(ui.info?.firmware.json_workspace_bytes)}}</dd></div><div><dt>{{t('MCP: лимит запроса','MCP: request limit')}}</dt><dd>{{budget(ui.info?.firmware.mcp_input_bytes)}}</dd></div><div><dt>{{t('MCP: буфер ответа','MCP: response buffer')}}</dt><dd>{{budget(ui.info?.firmware.mcp_output_bytes)}}</dd></div><div><dt>{{t('Прошивка','Firmware')}}</dt><dd>{{ui.info?.firmware.version??'—'}}</dd></div><div><dt>Boot ID</dt><dd class="mono">{{ui.boot||'—'}}</dd></div></dl></section>
</div><DeviceDetails/></template>


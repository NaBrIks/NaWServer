<script setup lang="ts">
import {computed} from 'vue';
import type {Point,Transmission} from '../contracts';
import {t} from '../state/appearance';
import Icon from './Icon.vue';import InfoButton from './InfoButton.vue';import MiniTrend from './MiniTrend.vue';
const props=defineProps<{stats:Transmission;fresh:boolean;jobHistory:Point[];memoryHistory:Point[];rateHistory:Point[]}>();
const number=(value:number|bigint)=>value.toLocaleString(t('ru-RU','en-US'));
const kib=(value:number|bigint)=>typeof value==='bigint'?`${number((value*10n+512n)/1024n/10n)}${t(',','.')}${(value*10n+512n)/1024n%10n}`:(value/1024).toLocaleString(t('ru-RU','en-US'),{minimumFractionDigits:1,maximumFractionDigits:1});
const jobs=computed(()=>[
 [t('Заполняются','Filling'),props.stats.filling_jobs],[t('В очереди','Queued'),props.stats.queued_jobs],[t('Передаются','Active'),props.stats.active_jobs],
 [t('Предел заданий','Job limit'),props.stats.maximum_jobs],[t('На соединение','Per connection'),props.stats.maximum_jobs_per_connection],[t('Резерв важных','Important reserve'),props.stats.reserved_important_jobs]
]);
const memory=computed(()=>[
 [t('Занято','Used'),props.stats.used_bytes],[t('Выделено','Allocated'),props.stats.allocated_bytes],[t('Пик','Peak'),props.stats.peak_bytes],
 [t('Предел','Limit'),props.stats.maximum_bytes],[t('Блок','Block'),props.stats.block_bytes],[t('Ожидают передачи','Pending bytes'),props.stats.queued_bytes]
]);
const counters=computed(()=>[
 [t('Завершено','Completed'),props.stats.completed_jobs],[t('Ошибки','Failed'),props.stats.failed_jobs],[t('Отменено','Discarded'),props.stats.discarded_jobs],
 [t('Не принято','Rejected'),props.stats.rejected_jobs],[t('Нет места в TCP','TCP would block'),props.stats.would_block_count]
]);
const rate=computed(()=>props.rateHistory.at(-1)?.value);
const help=computed(()=>t('Реальные показатели общего передатчика из HTTP/WS summary. Пределы показаны из конфигурации устройства. Скорость — разность sent_bytes между полученными образцами; sent означает принятие TCP-стеком, не подтверждение клиента. Графики заданий и памяти имеют шкалу от нуля до соответствующего лимита.','Actual shared transmitter metrics from HTTP/WS summary. Limits come from device configuration. Rate is the sent_bytes delta between received samples; sent means TCP-stack acceptance, not peer acknowledgement. Jobs and memory charts use zero-to-configured-limit scales.'));
const diagnostics=computed(()=>{
 const failure=props.stats.last_failure;
 const names=[t('Нет','None'),t('Истёк срок','Expired'),t('Нет прогресса','Stalled'),t('Ошибка записи TCP','Native write'),t('Отменено','Canceled')];
 return [{term:t('Статистика','Statistics'),description:help.value},
  {term:t('Задания','Jobs'),description:t('«В очереди» включает передающиеся задания; «Передаются» — подмножество этой строки. Заполняемые блоки учитываются отдельно.','Queued includes active writing jobs; Active is a subset of Queued. Filling blocks are counted separately.')},
  {term:t('Последний отказ','Last failure'),description:failure===undefined?'—':`${failure} · ${names[failure]}`},
  {term:t('Код ошибки платформы','Native error code'),description:props.stats.last_native_error===undefined?'—':String(props.stats.last_native_error)},
  {term:t('Шаг передачи','Transmission step'),description:props.stats.step_bytes===undefined?'—':`${number(props.stats.step_bytes)} B`},
  {term:t('Таймаут без прогресса','Stall timeout'),description:props.stats.stall_timeout_ms===undefined?'—':`${number(props.stats.stall_timeout_ms)} ms`},
  {term:t('Таймаут закрытия','Close timeout'),description:props.stats.close_timeout_ms===undefined?'—':`${number(props.stats.close_timeout_ms)} ms`},
  {term:t('Резерв / без копии','Reserve / borrowed'),description:t('Резерв — настроенные байты для важных передач. «Без копии» — текущие заимствованные неизменяемые диапазоны; они не входят в выделенную payload RAM.','Reserve is the configured byte allowance protected for important transfers. Borrowed counts current immutable externally owned spans; it is not allocated payload RAM.')}];
});
</script>
<template><section class="panel transmission-panel" data-testid="transmission-panel" :class="{stale:!fresh}">
 <div class="panel-head"><h2><Icon name="arrows-exchange"/>HTTP / WS / SSE · TX</h2><span :class="stats.enabled?'ok':''">{{stats.enabled?t('Общая очередь','Shared queue'):t('Выключено','Disabled')}}</span><InfoButton :label="t('Передача','Transmission')" :items="diagnostics"/></div>
 <div class="transmission-trends"><span>{{t('Задания','Jobs')}}<MiniTrend :points="jobHistory" :max="Math.max(1,stats.maximum_jobs)" :stale="!fresh" :label="t('Занятые задания / предел','Occupied jobs / limit')"/></span><span>{{t('Память','Memory')}}<MiniTrend :points="memoryHistory" :max="Math.max(1,stats.maximum_bytes)" :stale="!fresh" :label="t('Занятые байты / предел','Used bytes / limit')"/></span><span class="transmission-rate">TX <b>{{fresh&&rate!==undefined?kib(rate):'—'}}</b> {{t('КиБ/с','KiB/s')}}</span></div>
 <div class="transmission-scroll"><div class="transmission-tables">
  <table><thead><tr><th>{{t('Задания','Jobs')}}</th><th class="numeric">{{t('шт.','count')}}</th></tr></thead><tbody><tr v-for="row in jobs" :key="String(row[0])"><th scope="row">{{row[0]}}</th><td class="numeric">{{fresh?number(row[1] as number):'—'}}</td></tr></tbody></table>
  <table><thead><tr><th>{{t('Память','Memory')}}</th><th class="numeric">{{t('КиБ','KiB')}}</th></tr></thead><tbody><tr v-for="row in memory" :key="String(row[0])"><th scope="row">{{row[0]}}</th><td class="numeric" :data-tooltip="String(row[1])+' B'">{{fresh?kib(row[1] as number):'—'}}</td></tr></tbody></table>
  <table><thead><tr><th>{{t('Счётчики','Counters')}}</th><th class="numeric">{{t('шт.','count')}}</th></tr></thead><tbody><tr v-for="row in counters" :key="String(row[0])"><th scope="row">{{row[0]}}</th><td class="numeric">{{fresh?number(row[1] as number|bigint):'—'}}</td></tr><tr><th scope="row">{{t('Передано, КиБ','Sent, KiB')}}</th><td class="numeric" :data-tooltip="String(stats.sent_bytes)+' B'">{{fresh?kib(stats.sent_bytes):'—'}}</td></tr></tbody></table>
 </div></div>
 <div class="transmission-totals"><span>{{t('Принято','Accepted')}} <b>{{fresh?kib(stats.accepted_bytes):'—'}}</b></span><span>{{t('Отброшено','Discarded')}} <b>{{fresh?kib(stats.discarded_bytes):'—'}}</b></span><span>{{t('Ошибка','Failed')}} <b>{{fresh?kib(stats.failed_bytes):'—'}}</b></span><span>{{t('Резерв','Reserve')}} <b>{{fresh&&stats.reserved_important_bytes!==undefined?kib(stats.reserved_important_bytes):'—'}}</b></span><span>{{t('Без копии','Borrowed')}} <b>{{fresh&&stats.borrowed_bytes!==undefined?kib(stats.borrowed_bytes):'—'}}</b></span><span>{{t('КиБ','KiB')}}</span></div>
</section></template>

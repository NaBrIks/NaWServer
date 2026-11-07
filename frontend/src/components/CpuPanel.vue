<script setup lang="ts">
import {computed} from 'vue';
import {t} from '../state/appearance';
import Icon from './Icon.vue';
import type {Point} from '../contracts';
const props=defineProps<{total:number|null;cores:number[];totalHistory:Point[];coreHistory:Point[][];sampleInterval:number;fresh:boolean}>();
const end=computed(()=>props.totalHistory.at(-1)?.t??0);
const colors=['var(--lu-danger)','var(--lu-primary)','var(--lu-info)','var(--lu-warning)'];
const series=computed(()=>[
 ...props.cores.map((value,index)=>({id:`CPU${index}`,points:props.coreHistory[index]??[],value,color:colors[index%colors.length],dash:''})),
 {id:t('Общая','Total'),points:props.totalHistory,value:props.total??undefined,color:'var(--lu-text)',dash:'5 3'}
]);
function path(points:Point[]){return points.filter(p=>p.t>=end.value-60000).map((p,index)=>`${index?'L':'M'}${(48+(p.t-end.value+60000)/60000*680).toFixed(1)},${(150-Math.max(0,Math.min(100,p.value))*1.3).toFixed(1)}`).join(' ');}
const explanation=computed(()=>t('Каждое ядро: загрузка 0–100%. Общая загрузка предоставлена устройством в шкале 0–100%.','Each core: load 0–100%. Total load is reported by the device on a 0–100% scale.'));
</script>
<template><section class="panel" data-testid="cpu-panel"><div class="panel-head"><h2><Icon name="bolt"/>{{t('Загрузка CPU','CPU load')}}</h2><span>0–100% · {{cores.length}} cores · {{sampleInterval}} ms</span></div><div class="cpu-values" :data-tooltip="explanation"><span v-for="line in series" :key="line.id" :style="{color:line.color}">{{line.id}} <b>{{fresh&&line.value!==undefined?line.value.toFixed(1):'—'}}%</b></span></div><svg class="chart" viewBox="0 0 760 180" role="img" :aria-label="explanation" data-min="0" data-max="100"><g class="chart-grid"><path v-for="y in [20,63,107,150]" :key="y" :d="`M48 ${y}H736`"/></g><g class="chart-axis"><text x="0" y="24">100%</text><text x="0" y="90">50</text><text x="0" y="154">0</text><text x="48" y="176">−60 s</text><text x="680" y="176">{{t('сейчас','now')}}</text></g><path v-for="line in series" :key="line.id" :data-cpu="line.id" :d="path(line.points)" fill="none" :stroke="line.color" :stroke-dasharray="line.dash" stroke-width="1.7" stroke-linejoin="round"/></svg></section></template>

<script setup lang="ts">
import {computed} from 'vue';import type {Point} from '../contracts';
const props=withDefaults(defineProps<{points:Point[];label:string;max?:number;stale?:boolean}>(),{max:100});
const recent=computed(()=>{const end=props.points.at(-1)?.t??0;return props.points.filter(p=>p.t>=end-60000);});
const plotted=computed(()=>{const end=recent.value.at(-1)?.t??0;return recent.value.map(p=>({x:1+(p.t-end+60000)/60000*78,y:23-Math.max(0,Math.min(props.max,p.value))/Math.max(1,props.max)*21}));});
const path=computed(()=>plotted.value.map((p,i)=>`${i?'L':'M'}${p.x.toFixed(1)},${p.y.toFixed(1)}`).join(' '));
</script>
<template><svg :class="['mini-trend',{stale}]" viewBox="0 0 80 26" role="img" :aria-label="label" :data-tooltip="label" data-min="0" :data-max="max"><path class="mini-baseline" d="M1 24H79"/><path :d="path" class="mini-line"/><circle v-if="plotted.length" :cx="plotted.at(-1)!.x" :cy="plotted.at(-1)!.y" r="1.7"/></svg></template>

<script setup lang="ts">
import {computed} from 'vue';
import {combinedMemory,type MemoryRegion} from '../contracts/memory';
import {memoryLabel,memoryNumber} from '../state/memory-labels';
import {t} from '../state/appearance';
const props=defineProps<{regions:MemoryRegion[];fresh:boolean}>();
const combined=computed(()=>combinedMemory(props.regions));
const description=computed(()=>t('Учтённая RAM','Accounted RAM')+': '+combined.value.segments.map(segment=>`${memoryLabel(segment.kind)} ${memoryNumber(segment.bytes)} ${t('КиБ','KiB')}`).join('; '));
</script>
<template><svg class="memory-strip" :class="{stale:!fresh}" :viewBox="`0 0 ${combined.total||1} 6`" preserveAspectRatio="none" role="img" :aria-label="description" :data-tooltip="description" data-testid="memory-strip"><rect v-for="segment in combined.segments" :key="segment.kind" :class="`memory-color-${segment.kind}`" :x="segment.offset" y="0" :width="segment.bytes" height="6"/></svg></template>

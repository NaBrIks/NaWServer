<script setup lang="ts">
import {computed,onBeforeUnmount,ref,watchEffect} from 'vue';import {appearance,t} from '../state/appearance';import TasksPanel from '../features/TasksPanel.vue';import Icon from './Icon.vue';
const viewport=ref(window.innerWidth),drag=ref<{x:number;width:number;pointer:number}|null>(null);
const navWidth=computed(()=>viewport.value<=650?42:viewport.value<=950||appearance.collapsed?48:210);
const maximum=computed(()=>Math.max(260,Math.min(720,viewport.value-navWidth.value-280)));
const minimum=computed(()=>Math.min(320,maximum.value));
const width=computed(()=>Math.max(minimum.value,Math.min(maximum.value,appearance.tasksWidth)));
function resizeViewport(){viewport.value=window.innerWidth;}
window.addEventListener('resize',resizeViewport);
watchEffect(()=>document.documentElement.style.setProperty('--tasks-width',width.value+'px'));
function setWidth(value:number){appearance.tasksWidth=Math.round(Math.max(minimum.value,Math.min(maximum.value,value)));}
function down(event:PointerEvent){if(event.button!==0)return;event.preventDefault();(event.currentTarget as HTMLElement).setPointerCapture(event.pointerId);drag.value={x:event.clientX,width:width.value,pointer:event.pointerId};}
function move(event:PointerEvent){if(drag.value?.pointer===event.pointerId)setWidth(drag.value.width+drag.value.x-event.clientX);}
function up(event:PointerEvent){if(drag.value?.pointer!==event.pointerId)return;drag.value=null;const target=event.currentTarget as HTMLElement;if(target.hasPointerCapture(event.pointerId))target.releasePointerCapture(event.pointerId);}
function key(event:KeyboardEvent){const step=event.shiftKey?40:10;if(event.key==='ArrowLeft')setWidth(width.value+step);else if(event.key==='ArrowRight')setWidth(width.value-step);else if(event.key==='Home')setWidth(minimum.value);else if(event.key==='End')setWidth(maximum.value);else return;event.preventDefault();}
onBeforeUnmount(()=>window.removeEventListener('resize',resizeViewport));
</script>
<template><aside class="tasks-dock" :class="{resizing:drag}" :aria-label="t('Панель задач','Task panel')" data-testid="tasks-dock"><div class="tasks-resizer" role="separator" tabindex="0" aria-orientation="vertical" :aria-label="t('Ширина панели задач','Task panel width')" :aria-valuemin="minimum" :aria-valuemax="maximum" :aria-valuenow="width" :aria-valuetext="width+' px'" :data-tooltip="t('Ширина панели: тяните границу; ← / → — 10 px, Shift — 40 px, Home / End — пределы','Panel width: drag the divider; ← / → changes 10 px, Shift changes 40 px, Home / End selects limits')" @pointerdown="down" @pointermove="move" @pointerup="up" @pointercancel="up" @lostpointercapture="drag=null" @keydown="key" @dblclick="setWidth(480)"><Icon name="grip-horizontal"/></div><TasksPanel/></aside></template>

<script setup lang="ts">
import {computed,ref} from 'vue';
import {appearance,t} from '../state/appearance';
import Icon from './Icon.vue';
const menu=ref<HTMLDetailsElement>();
const themes=computed(()=>[
 {id:'light',name:t('Светлая','Light'),icon:'sun'},
 {id:'cream',name:t('Кремовая','Cream'),icon:'ice-cream'},
 {id:'emerald',name:t('Изумрудная','Emerald'),icon:'trees'},
 {id:'dark',name:t('Тёмная','Dark'),icon:'moon'}]);
const densities=computed(()=>[
 {id:'nano',name:t('Ультракомпактная','Ultra compact'),icon:'baseline-density-small'},
 {id:'mini',name:t('Компактная','Compact'),icon:'baseline-density-medium'},
 {id:'norm',name:t('Обычная','Normal'),icon:'baseline-density-large'}]);
const themeName=computed(()=>themes.value.find(item=>item.id===appearance.theme)?.name);
function close(){if(menu.value){menu.value.open=false;menu.value.querySelector('summary')?.focus();}}
</script>
<template>
 <details ref="menu" class="appearance" @keydown.esc.stop.prevent="close">
  <summary :aria-label="t('Оформление','Appearance')"><Icon name="palette"/> {{themeName}} · {{appearance.density.toUpperCase()}}</summary>
  <div class="appearance-menu">
   <strong>{{t('Цветовая схема','Color scheme')}}</strong>
   <button v-for="item in themes" :key="item.id" :class="{selected:appearance.theme===item.id}" :aria-label="item.name" :aria-pressed="appearance.theme===item.id" @click="appearance.theme=item.id"><Icon :name="item.icon"/>{{item.name}}<span v-if="appearance.theme===item.id" aria-hidden="true">✓</span></button>
   <strong>{{t('Плотность','Density')}}</strong>
   <button v-for="item in densities" :key="item.id" :class="{selected:appearance.density===item.id}" :aria-label="item.name" :aria-pressed="appearance.density===item.id" @click="appearance.density=item.id"><Icon :name="item.icon"/>{{item.name}}<span v-if="appearance.density===item.id" aria-hidden="true">✓</span></button>
  </div>
 </details>
</template>

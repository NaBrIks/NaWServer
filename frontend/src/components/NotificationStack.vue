<script setup lang="ts">
import {computed,ref} from 'vue';
import {notices,dismissNotice,clearNotices} from '../state/notifications';
import {t} from '../state/appearance';
const dialog=ref<HTMLDialogElement|null>(null),trigger=ref<HTMLButtonElement|null>(null);
const severity=computed(()=>['error','warning','success','info'].find(kind=>notices.some(item=>item.kind===kind))??'');
</script>
<template>
 <button ref="trigger" class="notice-indicator" :class="severity" :aria-label="t('Сообщения','Messages')" :data-tooltip="t('Сообщения','Messages')+' · '+notices.length" data-testid="notifications" @click="dialog?.showModal()">ⓘ</button>
 <dialog ref="dialog" class="notice-dialog" @close="trigger?.focus()" data-testid="notification-details">
  <div class="panel-head"><h2>{{t('Сообщения','Messages')}}</h2><button class="notice-action" :aria-label="t('Закрыть','Close')" @click="dialog?.close()">×</button></div>
  <p v-if="!notices.length">{{t('Нет сообщений','No messages')}}</p>
  <div v-for="item in notices" :key="item.id" class="notice-row" :data-kind="item.kind"><span :class="item.kind">{{item.message}}</span><button class="notice-action" :aria-label="t('Удалить сообщение','Dismiss message')" @click="dismissNotice(item.id)">×</button></div>
  <button class="text-button" data-testid="notifications-close-all" @click="clearNotices();dialog?.close()">{{t('Очистить','Clear')}}</button>
 </dialog>
</template>

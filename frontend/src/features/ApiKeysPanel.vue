<script setup lang="ts">
import {nextTick,onBeforeUnmount,ref} from 'vue';
import {ApiKeyAdmin,type ApiKeySlot} from '../client/api_keys';
import {commandCredentials} from '../state/access';
import {notify} from '../state/notifications';
import {t} from '../state/appearance';
import Icon from '../components/Icon.vue';
import InfoButton from '../components/InfoButton.vue';
function validName(name:string){return name.length>0&&new TextEncoder().encode(name).length<=24&&!/[\u0000-\u001f\u007f]/.test(name);}
const admin=new ApiKeyAdmin(),busy=ref(false),loaded=ref(false),secret=ref(''),secretSlot=ref(0),dialog=ref<HTMLDialogElement>();
const slots=ref<ApiKeySlot[]>(Array.from({length:4},(_,slot)=>({slot,name:'',active:false,revision:0})));
async function reload(){busy.value=true;try{const value=await admin.list(commandCredentials.user,commandCredentials.password);slots.value=Array.from({length:value.maximum_slots},(_,slot)=>value.slots.find(item=>item.slot===slot)??{slot,name:'',active:false,revision:0});loaded.value=true;notify(t('Ключи обновлены','Keys refreshed'),'success');}catch(error){notify(String(error),'error');}finally{busy.value=false;}}
async function change(action:'create'|'rotate'|'revoke',row:ApiKeySlot){
 busy.value=true;secret.value='';
 try{const result=await admin.change(action,row.slot,row.name,commandCredentials.user,commandCredentials.password);await reload();
  if(result.secret){secret.value=result.secret;secretSlot.value=result.slot+1;await nextTick();dialog.value?.showModal();}
  notify(t('Ключ сохранён','Key saved'),'success');
 }catch(error){notify(String(error),'error');}finally{busy.value=false;}
}
function clearSecret(){secret.value='';}
async function copy(){try{await navigator.clipboard.writeText(secret.value);notify(t('Ключ скопирован','Key copied'),'success');}catch{notify(t('Не удалось скопировать: выделите ключ вручную','Copy unavailable: select the key manually'),'error');}}
onBeforeUnmount(clearSecret);
</script>
<template>
<section class="panel api-keys-panel" data-testid="api-keys-panel">
 <div class="panel-head"><h2><Icon name="lock"/>{{t('Приоритетные ключи API','Priority API keys')}}</h2><button :disabled="busy" data-testid="keys-load" @click="reload">{{t('Прочитать','Read')}}</button><InfoButton :label="t('Ключи API','API keys')" :text="t('Ключи хранятся на устройстве. Управление использует учётную запись выше. Секрет показывается только при создании или замене; список содержит только имя и ревизию. Замена и отзыв запрещают новые запросы старого ключа. Уже принятый запрос завершается по обычным лимитам. API-клиент передаёт секрет в X-Client-Key. Метка страницы page_id несекретна и создаётся заново при загрузке документа.','Keys persist on the device. Management uses the credentials above. A secret is shown only when created or rotated; the list contains name and revision only. Rotation and revocation deny new requests using the old key. Existing requests finish within normal bounds. API clients send the secret in X-Client-Key. The non-secret page_id label is fresh for each document load.')"/></div>
 <div class="table-wrap"><table class="api-keys-table" data-testid="keys-table"><thead><tr><th>№</th><th>{{t('Имя','Name')}}</th><th>{{t('Состояние','State')}}</th><th>{{t('Ревизия','Revision')}}</th><th>{{t('Действия','Actions')}}</th></tr></thead><tbody><tr v-for="row in slots" :key="row.slot"><td class="mono">{{row.slot+1}}</td><td><input v-model="row.name" :disabled="busy||!loaded" :aria-label="t('Имя ключа ','Key name ')+(row.slot+1)" maxlength="24" autocomplete="off" :title="row.name" :aria-invalid="!validName(row.name)"></td><td :class="row.active?'ok':''">{{!loaded?'—':row.active?t('Активен','Active'):t('Свободен','Empty')}}</td><td class="mono">{{loaded?row.revision:'—'}}</td><td><button v-if="!row.active" :disabled="busy||!loaded||row.slot!==slots.find(item=>!item.active)?.slot||!validName(row.name)" @click="change('create',row)">{{t('Создать','Create')}}</button><template v-else><button :disabled="busy" @click="change('rotate',row)">{{t('Заменить','Rotate')}}</button><button :disabled="busy" @click="change('revoke',row)">{{t('Отозвать','Revoke')}}</button></template></td></tr></tbody></table></div>
</section>
<dialog ref="dialog" class="help-dialog api-key-secret" @close="clearSecret" @cancel="clearSecret"><header><strong>{{t('Новый ключ','New key')}} {{secretSlot}}</strong><button class="icon-button" :aria-label="t('Закрыть','Close')" @click="dialog?.close()"><Icon name="x"/></button></header><div class="dialog-body"><label>{{t('Сохраните сейчас: повторно прочитать нельзя','Save now: it cannot be read again')}}<input :value="secret" readonly spellcheck="false" autocomplete="off" data-testid="key-secret" @focus="($event.target as HTMLInputElement).select()"></label></div><form method="dialog"><button type="button" @click="copy">{{t('Копировать','Copy')}}</button><button>{{t('Закрыть','Close')}}</button></form></dialog>
</template>

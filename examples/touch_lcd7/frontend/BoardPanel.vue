<script setup lang="ts">
import {computed,ref} from 'vue';
import type {Summary} from '@frontend/contracts';
import type {DisplayState} from './display';
import {ui,request,notify,formatBytes} from '@frontend/state/device';
import {t} from '@frontend/state/appearance';
import Icon from '@frontend/components/Icon.vue';
import InfoButton from '@frontend/components/InfoButton.vue';
const network=computed(()=>ui.summary?.network);
const display=computed(()=>(ui.summary as (Summary & {display?:DisplayState})|null)?.display),busy=ref(false);
const modes=computed(()=>[{command:'bars',name:t('Полосы','Color bars')},{command:'grid',name:t('Сетка','Grid')},{command:'red',name:t('Красный','Red')},{command:'green',name:t('Зелёный','Green')},{command:'blue',name:t('Синий','Blue')}]);
async function command(value:string){busy.value=true;try{const r=await request('/api/display',{method:'POST',headers:{'Content-Type':'text/plain','X-LCD7-Command':'1'},body:value});if(r.status!==202)throw Error(`${r.status}: ${r.text}`);notify(t('Команда дисплея поставлена в очередь','Display command queued'),'success');}catch(error){notify(String(error),'error')}finally{busy.value=false}}
const ready=computed(()=>ui.httpOnline&&!!display.value?.ready);
const psram=computed(()=>ui.summary?.memory.find(pool=>pool.id==='psram'));

const boardHelp=computed(()=>[
  ['Framebuffer','Framebuffer','Два полноэкранных буфера RGB565 находятся в PSRAM.','Two full-screen RGB565 buffers reside in PSRAM.'],
  ['LVGL','LVGL','Рисует в неактивном буфере, который сейчас не выводится на LCD.','Renders into the inactive buffer, not the one currently scanned out.'],
  ['Frame completion / VSYNC','Frame completion / VSYNC','Окончание кадра и следующий VSYNC разрешают повторное использование предыдущего буфера.','Frame completion and the following VSYNC permit reuse of the previous buffer.'],
  ['GT911','GT911','Сенсор физических касаний. Карта показывает координаты, а не изображение LCD.','Physical touch sensor. The map shows coordinates, not an LCD screenshot.']
].map(([ru,en,ruText,enText])=>({term:t(ru,en),description:t(ruText,enText)})));

const touchHelp=computed(()=>[
  ['Координаты','Coordinates','Физические данные GT911 в общей CBOR-сводке.','Physical GT911 input in the shared CBOR summary.'],
  ['WebSocket','WebSocket','При движении пальца частота сводки повышается до 10 Гц.','While a finger moves, summaries increase to up to 10 Hz.'],
  ['HTTP','HTTP','Резервный опрос состояния.','Fallback state polling.'],
  ['Верхняя полоса LCD','LCD top strip','Касание переключает тест изображения на физическом экране.','Touch switches the physical display test pattern.']
].map(([ru,en,ruText,enText])=>({term:t(ru,en),description:t(ruText,enText)})));
</script>
<template>
 <div class="board-summary-panels">
 <section class="panel"><div class="panel-head"><h2><Icon name="device-desktop"/>Waveshare ESP32-S3-Touch-LCD-7</h2><InfoButton :label="t('Плата и экран','Board and display')" :items="boardHelp"/></div>
 <div class="table-wrap"><table><thead><tr><th>{{t('Узел','Component')}}</th><th>{{t('Конфигурация','Configuration')}}</th><th class="numeric">{{t('Значение','Value')}}</th></tr></thead><tbody>
 <tr><td>ESP32-S3</td><td>{{t('Два ядра · Flash 16 МиБ · PSRAM 8 МиБ','Dual core · Flash 16 MiB · PSRAM 8 MiB')}}</td><td class="numeric">{{ui.info?.chip.cores??'—'}}</td></tr>
 <tr><td>LCD / LVGL</td><td>{{display?.renderer??'—'}} · RGB565</td><td class="numeric">{{display?.width??800}} × {{display?.height??480}}</td></tr>
 <tr><td>{{t('Кадровые буферы','Framebuffers')}}</td><td>PSRAM · frame complete + VSYNC</td><td class="numeric">{{display?.framebuffers??'—'}}</td></tr>
 <tr><td>PSRAM</td><td>{{t('Свободно / всего','Free / total')}}</td><td class="numeric">{{psram?formatBytes(psram.free_bytes)+' / '+formatBytes(psram.total_bytes):'—'}}</td></tr>
 <tr><td>{{t('Выведенные кадры','Frame swaps')}}</td><td>LVGL flush ready</td><td class="numeric" data-testid="display-swaps">{{display?.frame_swaps??'—'}}</td></tr>
 <tr><td>Frame / VSYNC</td><td>{{t('События RGB-драйвера','RGB driver events')}}</td><td class="numeric">{{display?.frame_events??'—'}} / {{display?.vsyncs??'—'}}</td></tr>
 <tr><td>GT911</td><td>SDA8 / SCL9 · GPIO4 / CH422G</td><td class="numeric">{{display?.touch_errors??'—'}} {{t('ошибок','errors')}}</td></tr>
 <tr><td>WS2812</td><td>Sensor AD · AD → DIN · GPIO6 · GRB</td><td class="numeric">10 × RGB</td></tr>
 </tbody></table></div></section>
 <section class="panel" data-testid="network-budget"><div class="panel-head"><h2>{{t('Сеть · лимиты и резерв','Network · limits and reserve')}}</h2><InfoButton :label="t('Лимиты соединений','Connection limits')" help-key="network"/></div>
 <div class="table-wrap"><table><thead><tr><th>{{t('Параметр','Metric')}}</th><th>{{t('Смысл','Meaning')}}</th><th class="numeric">{{t('Значение','Value')}}</th></tr></thead><tbody>
 <tr><td>lwIP</td><td>CONFIG_LWIP_MAX_SOCKETS</td><td class="numeric">{{network?.socket_limit??'—'}}</td></tr>
 <tr><td>TCP PCB</td><td>CONFIG_LWIP_MAX_ACTIVE_TCP</td><td class="numeric">{{network?.tcp_pcb_limit??'—'}}</td></tr>
 <tr><td>HTTPD</td><td>{{t('Служебные сокеты','Internal sockets')}}</td><td class="numeric">{{network?.reserved_sockets??'—'}}</td></tr>
 <tr><td>TCP</td><td>{{t('Занято / предел HTTPD','Open / HTTPD limit')}}</td><td class="numeric" data-testid="tcp-usage">{{network?.httpd_open??'—'}} / {{network?.httpd_limit??'—'}}</td></tr>
 <tr><td>{{t('Резерв отказа','Rejection reserve')}}</td><td>{{t('Ответ 503 + Retry-After; порог приёма','503 + Retry-After; admission threshold')}}</td><td class="numeric">{{network?.http_reserve??'—'}} · {{network?network.httpd_limit-network.http_reserve:'—'}}</td></tr>
 <tr><td>WS</td><td>{{t('Клиенты / предел','Clients / limit')}}</td><td class="numeric">{{network?.ws_clients??'—'}} / {{network?.ws_limit??'—'}}</td></tr>
 <tr><td>SSE</td><td>{{t('Клиенты / предел','Clients / limit')}}</td><td class="numeric">{{network?.sse_clients??'—'}} / {{network?.sse_limit??'—'}}</td></tr>
 <tr><td>{{t('Отказы','Rejected')}}</td><td>{{network?.last_rejection??'—'}}</td><td class="numeric" data-testid="admission-rejected">{{network?.admission_rejected??'—'}}</td></tr>
 <tr><td>Retry-After</td><td>{{t('Рекомендуемая задержка, мс','Recommended delay, ms')}}</td><td class="numeric">{{network?.retry_after_ms??'—'}}</td></tr>
 </tbody></table></div></section>
 </div>
 <section class="panel"><div class="panel-head"><h2>{{t('Дисплей и подсветка','Display and backlight')}}</h2><span :class="ready?'ok':'bad'">{{ready?t('Готов','Ready'):t('Нет свежих данных','No fresh data')}}</span></div>
 <div class="effect-buttons" role="group" :aria-label="t('Режим дисплея','Display mode')"><button v-for="(mode,index) in modes" :key="mode.command" :disabled="!ready||busy" :aria-pressed="display?.pattern===index" :class="{active:display?.pattern===index}" @click="command(mode.command)">{{mode.name}}</button></div>
 <div class="inline-actions"><span>{{t('Подсветка','Backlight')}}: <b>{{display?.backlight?t('включена','on'):t('выключена','off')}}</b></span><button :disabled="!ready||busy" @click="command('on')"><Icon name="sun"/>{{t('Включить','On')}}</button><button :disabled="!ready||busy" @click="command('off')"><Icon name="moon"/>{{t('Выключить','Off')}}</button></div></section>
 <section class="panel"><div class="panel-head"><h2>{{t('Сенсор · физические касания','Touch · physical input')}}</h2><InfoButton :label="t('Карта касаний','Touch map')" :items="touchHelp"/></div>
 <div class="board-touch-values"><span>X <b>{{display?.x??'—'}}</b></span><span>Y <b>{{display?.y??'—'}}</b></span><span>{{t('Касания','Touches')}} <b data-testid="touch-count">{{display?.touches??'—'}}</b></span><span :class="display?.pressed?'ok':'muted'">{{display?.pressed?t('Нажато','Pressed'):t('Отпущено','Released')}}</span></div>
 <svg class="board-touch-map" viewBox="0 0 800 480" role="img" :aria-label="t('Координаты GT911','GT911 coordinates')"><line v-for="x in 15" :key="'x'+x" :x1="x*50" y1="0" :x2="x*50" y2="480"/><line v-for="y in 9" :key="'y'+y" x1="0" :y1="y*50" x2="800" :y2="y*50"/><rect x="0" y="0" width="800" height="100" class="board-touch-header"/><circle v-if="display?.pressed" :cx="display.x" :cy="display.y" r="12"/></svg>
 <div class="endpoint">{{t('Карта координат, не видеопоток LCD','Coordinate map, not an LCD video feed')}} · WS / CBOR v2</div></section>
 <section class="panel"><div class="panel-head"><h2>{{t('Разъёмы и ограничения','Connectors and constraints')}}</h2></div><div class="table-wrap"><table><thead><tr><th>{{t('Разъём','Connector')}}</th><th>{{t('Выводы / назначение','Pins / assignment')}}</th></tr></thead><tbody><tr><td>Sensor AD</td><td>AD = GPIO6 → WS2812 DIN; GND → GND; {{t('питание ленты отдельно по её спецификации','strip power per its specification')}}</td></tr><tr><td>I²C / touch</td><td>GPIO8 / GPIO9 · GT911 + CH422G</td></tr><tr><td>RGB LCD</td><td>{{t('GPIO2 занят LCD; не подключать туда ленту','GPIO2 belongs to LCD; do not connect the strip there')}}</td></tr><tr><td>Sensor AD / RGB</td><td>{{t('Не подключать аналоговый датчик одновременно с RGB DIN','Do not attach an analog sensor alongside RGB DIN')}}</td></tr><tr><td>CAN / RS485 / SD</td><td>{{t('Не включены этой демкой','Not enabled by this demo')}}</td></tr></tbody></table></div></section>
</template>

<style>
.board-touch-values{display:flex;gap:16px;padding:6px 0;font-variant-numeric:tabular-nums}.board-touch-values b{display:inline-block;min-width:4ch;text-align:right}.board-touch-map{display:block;width:100%;max-height:340px;aspect-ratio:5/3;background:var(--lu-surface);border:1px solid var(--lu-border)}.board-touch-map line{stroke:var(--lu-border);stroke-width:1}.board-touch-map .board-touch-header{fill:var(--lu-primary);opacity:.08}.board-touch-map circle{fill:var(--lu-primary);stroke:var(--lu-text);stroke-width:2}
.board-summary-panels{display:flex;align-items:flex-start;gap:var(--gap);min-width:0}.board-summary-panels>.panel{flex:1 1 0;min-width:0;width:calc((100% - var(--gap))/2)}
</style>

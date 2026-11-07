import type {FrontendProject} from '@frontend/project';
import StoragePanel from './StoragePanel.vue';
export const project:FrontendProject={
 label:'PICO 2 W',
 storageKey:'nawserver.pico2w.appearance',
 admissionPreflight:false,
 platform:{deviceLabel:'Pico 2 W',chipFallback:'RP2350',sdkLabel:'Pico SDK',
  internalMemoryLabel:{ru:'FreeRTOS heap',en:'FreeRTOS heap'},workloadPriorityMax:2,ledController:'CYW43439',
  detailLabels:{active_cores:['Активных ядер','Active cores'],LittleFS_bytes:['LittleFS','LittleFS'],NVS_bytes:['NVS','NVS'],heap_allocator:['Аллокаторы','Allocators'],LED_controller:['Контроллер LED','LED controller'],USB_BOOTSEL:['USB / перезагрузка','USB / reboot']}},
 pages:[{id:'storage',icon:'database',label:{ru:'Flash / настройки',en:'Flash / settings'},component:StoragePanel}]
};

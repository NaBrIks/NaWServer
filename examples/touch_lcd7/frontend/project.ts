import {defineAsyncComponent} from 'vue';
import {espPlatform,type FrontendProject} from '@frontend/project';
export const project:FrontendProject={
 label:'TOUCH LCD 7',
 storageKey:'nawserver.touch-lcd7.appearance',
 platform:espPlatform,
 admissionPreflight:true,
 pages:[{id:'board',icon:'device-desktop',label:{ru:'Плата и дисплей',en:'Board and display'},component:defineAsyncComponent(()=>import('./BoardPanel.vue'))}]
};

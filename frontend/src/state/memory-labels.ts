import {t} from './appearance';
import type {MemoryKind} from '../contracts/memory';
export function memoryLabel(kind:MemoryKind):string {
 const labels:Record<MemoryKind,[string,string]>={own_static_bytes:['Наша статика','Our static'],other_static_bytes:['Прочая статика','Other static'],own_dynamic_bytes:['Наши буферы','Our buffers'],other_dynamic_bytes:['Прочая динамика','Other dynamic'],free_bytes:['Свободно','Free'],unknown_bytes:['Без разбивки','Unclassified']};
 return t(...labels[kind]);
}
export const memoryNumber=(bytes:number)=>(bytes/1024).toLocaleString(t('ru-RU','en-US'),{minimumFractionDigits:1,maximumFractionDigits:1});
export const regionLabel=(id:string)=>id==='internal'?t('Внутренняя RAM','Internal RAM'):id==='psram'?'PSRAM':id;

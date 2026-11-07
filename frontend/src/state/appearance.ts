import {project} from '@project';
import {reactive,watch} from 'vue';
const defaults={theme:'cream',density:'nano',language:'ru',collapsed:false,tasksWidth:480};
let saved:Partial<typeof defaults>={};try{saved=JSON.parse(localStorage.getItem(project.storageKey)??'{}');}catch{}
export const appearance=reactive({...defaults,...saved});
if(!['cream','dark','emerald','light'].includes(appearance.theme))appearance.theme='cream';
if(!['nano','mini','norm'].includes(appearance.density))appearance.density='nano';
if(!['ru','en'].includes(appearance.language))appearance.language='ru';
if(!Number.isFinite(appearance.tasksWidth))appearance.tasksWidth=480;
appearance.tasksWidth=Math.max(260,Math.min(720,appearance.tasksWidth));
watch(appearance,value=>{document.documentElement.dataset.theme=value.theme;document.documentElement.dataset.density=value.density;document.documentElement.lang=value.language;try{localStorage.setItem(project.storageKey,JSON.stringify(value));}catch{}},{immediate:true});
export function t(ru:string,en:string){return appearance.language==='ru'?ru:en;}

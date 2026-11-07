import {reactive} from 'vue';
export type NoticeKind='success'|'info'|'warning'|'error';
export interface Notice {id:number;key?:string;message:string;kind:NoticeKind}
// Bounded history is retained until the user opens/clears it. Messages never
// open a panel or disappear before the user has a chance to inspect them.
export const notices=reactive<Notice[]>([]);
let nextId=0;
export function dismissNotice(id:number){const index=notices.findIndex(item=>item.id===id);if(index>=0)notices.splice(index,1);}
export function dismissNoticeKey(key:string){const item=notices.find(item=>item.key===key);if(item)dismissNotice(item.id);}
export function clearNotices(){notices.splice(0);}
export function notify(message:string,kind:NoticeKind='info',options:{key?:string}={}){
 const existing=options.key?notices.find(item=>item.key===options.key):notices.find(item=>item.message===message&&item.kind===kind);
 if(existing){existing.message=message;existing.kind=kind;return existing.id;}
 if(notices.length>=32)notices.shift();
 const id=++nextId;notices.push({id,key:options.key,message,kind});return id;
}

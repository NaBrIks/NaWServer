export interface MemoryUsageRegion {
 id:string;total_bytes:number;free_bytes:number;own_static_bytes:number;other_static_bytes:number;own_dynamic_bytes:number;other_dynamic_bytes:number;
}
export const memoryKinds=['own_static_bytes','other_static_bytes','own_dynamic_bytes','other_dynamic_bytes','free_bytes','unknown_bytes'] as const;
export type MemoryKind=typeof memoryKinds[number];
export interface MemoryRegion {id:string;total:number;measured:boolean;segments:{kind:MemoryKind;bytes:number;offset:number}[]}
const byteCount=(value:unknown):value is number=>typeof value==='number'&&Number.isSafeInteger(value)&&value>=0;
export function isMemoryUsageRegion(value:unknown):value is MemoryUsageRegion {
 if(!value||typeof value!=='object')return false;
 const record=value as Record<string,unknown>;
 return typeof record.id==='string'&&byteCount(record.total_bytes)&&memoryKinds.slice(0,5).every(key=>byteCount(record[key]))&&
  memoryKinds.slice(0,5).reduce((sum,key)=>sum+Number(record[key]),0)===record.total_bytes;
}
export function memoryRegions(summary:{memory:{id:string;total_bytes:number;free_bytes:number}[];memory_usage?:MemoryUsageRegion[]}|null):MemoryRegion[] {
 if(!summary)return [];
 const measured=summary.memory_usage?.filter(region=>region.total_bytes>0);
 const regions=measured?.length?measured:summary.memory.filter(pool=>pool.total_bytes>0).map(pool=>({id:pool.id,total_bytes:pool.total_bytes,free_bytes:Math.min(pool.total_bytes,Math.max(0,pool.free_bytes))}));
 return regions.map(region=>{
  const complete=isMemoryUsageRegion(region);let offset=0;
  const segments=memoryKinds.map(kind=>{
   const bytes=kind==='unknown_bytes'?(complete?0:region.total_bytes-region.free_bytes):kind==='free_bytes'?region.free_bytes:complete?region[kind]:0;
   const segment={kind,bytes,offset};offset+=bytes;return segment;
  }).filter(segment=>segment.bytes>0);
  return {id:region.id,total:region.total_bytes,measured:complete,segments};
 });
}
export function combinedMemory(regions:MemoryRegion[]):MemoryRegion {
 let offset=0;
 const segments=memoryKinds.map(kind=>{
  const bytes=regions.reduce((sum,region)=>sum+(region.segments.find(segment=>segment.kind===kind)?.bytes??0),0);
  const segment={kind,bytes,offset};offset+=bytes;return segment;
 }).filter(segment=>segment.bytes>0);
 return {id:'all',total:offset,measured:regions.every(region=>region.measured),segments};
}

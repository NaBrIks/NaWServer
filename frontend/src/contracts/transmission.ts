export type TransmissionCounter=number|bigint;
export interface Transmission {
 enabled:boolean;
 maximum_jobs:number;maximum_bytes:number;block_bytes:number;maximum_jobs_per_connection:number;reserved_important_jobs:number;
 queued_jobs:number;filling_jobs:number;active_jobs:number;allocated_bytes:number;used_bytes:number;peak_bytes:number;queued_bytes:number;
 accepted_bytes:TransmissionCounter;sent_bytes:TransmissionCounter;discarded_bytes:TransmissionCounter;failed_bytes:TransmissionCounter;
 completed_jobs:TransmissionCounter;discarded_jobs:TransmissionCounter;rejected_jobs:TransmissionCounter;failed_jobs:TransmissionCounter;would_block_count:TransmissionCounter;
 last_failure?:0|1|2|3|4;last_native_error?:number;
 reserved_important_bytes?:TransmissionCounter;borrowed_bytes?:TransmissionCounter;
 step_bytes?:number;stall_timeout_ms?:number;close_timeout_ms?:number;
}
const sizes=['maximum_jobs','maximum_bytes','block_bytes','maximum_jobs_per_connection','reserved_important_jobs','queued_jobs','filling_jobs','active_jobs','allocated_bytes','used_bytes','peak_bytes','queued_bytes'];
const counters=['accepted_bytes','sent_bytes','discarded_bytes','failed_bytes','completed_jobs','discarded_jobs','rejected_jobs','failed_jobs','would_block_count'];
const counter=(value:unknown)=>typeof value==='bigint'?value>=0n:typeof value==='number'&&Number.isSafeInteger(value)&&value>=0;
export function isTransmission(value:unknown):value is Transmission{
 if(!value||typeof value!=='object'||Array.isArray(value))return false;
 const item=value as Record<string,unknown>;
 return typeof item.enabled==='boolean'&&sizes.every(key=>typeof item[key]==='number'&&Number.isSafeInteger(item[key])&&Number(item[key])>=0)
  &&counters.every(key=>counter(item[key]))
  &&['reserved_important_bytes','borrowed_bytes'].every(key=>item[key]===undefined||counter(item[key]))
  &&['step_bytes','stall_timeout_ms','close_timeout_ms'].every(key=>item[key]===undefined||(typeof item[key]==='number'&&Number.isSafeInteger(item[key])&&Number(item[key])>=0))
  &&(item.last_failure===undefined||(typeof item.last_failure==='number'&&Number.isInteger(item.last_failure)&&item.last_failure>=0&&item.last_failure<=4))
  &&(item.last_native_error===undefined||(typeof item.last_native_error==='number'&&Number.isSafeInteger(item.last_native_error)));
}
// Convert only an exactly representable delta. Total counters stay bigint and
// are rendered exactly; counter reset/wrap produces an unavailable rate.
export function transmissionRate(current:TransmissionCounter,previous:TransmissionCounter,elapsedMs:number):number|null{
 if(!Number.isFinite(elapsedMs)||elapsedMs<=0)return null;
 const delta=BigInt(current)-BigInt(previous);
 return delta<0n||delta>BigInt(Number.MAX_SAFE_INTEGER)?null:Number(delta)*1000/elapsedMs;
}
// queued_jobs already includes the currently writing subset active_jobs.
export function occupiedTransmissionJobs(stats:Pick<Transmission,'queued_jobs'|'filling_jobs'>){return stats.queued_jobs+stats.filling_jobs;}

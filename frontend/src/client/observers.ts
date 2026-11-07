export interface ObserverPreferences {ws:boolean;http:boolean;sse:boolean}
export const defaultObservers:ObserverPreferences={ws:true,http:false,sse:true};
export function observerStorageKey(namespace:string,deviceUrl:string){return `${namespace}.observers.${new URL(deviceUrl).origin}`;}
export function decodeObservers(raw:string|null):ObserverPreferences{
 let value:Partial<ObserverPreferences>={};try{value=JSON.parse(raw??'{}')??{};}catch{}
 return {ws:typeof value.ws==='boolean'?value.ws:defaultObservers.ws,http:typeof value.http==='boolean'?value.http:defaultObservers.http,sse:typeof value.sse==='boolean'?value.sse:defaultObservers.sse};
}

// An explicit opt-in observer. Disabling aborts its in-flight request and revokes
// the generation, so a late finally/rejection cannot schedule another poll.
export class SnapshotPoller {
 private enabled=false;
 private generation=0;
 private timer:ReturnType<typeof setTimeout>|undefined;
 private controller:AbortController|null=null;
 private readonly snapshot:(signal:AbortSignal)=>Promise<void>;
 private readonly intervalMs:number;
 constructor(snapshot:(signal:AbortSignal)=>Promise<void>,intervalMs=1000){this.snapshot=snapshot;this.intervalMs=intervalMs;}
 setEnabled(enabled:boolean,immediate=true){
  if(this.enabled===enabled)return;
  this.enabled=enabled;const generation=++this.generation;
  if(this.timer!==undefined)clearTimeout(this.timer);this.timer=undefined;
  this.controller?.abort();this.controller=null;
  if(enabled)this.timer=setTimeout(()=>void this.run(generation),immediate?0:this.intervalMs);
 }
 private async run(generation:number){
  if(!this.enabled||generation!==this.generation)return;
  this.timer=undefined;const controller=this.controller=new AbortController();
  try{await this.snapshot(controller.signal);}catch{ /* Snapshot owner reports errors. */ }
  finally{
   if(this.controller===controller)this.controller=null;
   if(this.enabled&&generation===this.generation)this.timer=setTimeout(()=>void this.run(generation),this.intervalMs);
  }
 }
}

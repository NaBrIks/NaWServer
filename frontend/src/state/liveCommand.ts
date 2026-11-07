import {onBeforeUnmount,ref} from 'vue';

// One in-flight command and one replaceable pending draft. Sliders send a leading
// sample and the final value, at most 10 Hz; clicks bypass the interval when idle.
export function useLiveCommand<T>(send:(value:T)=>Promise<void>){
 const sending=ref(false),pending=ref(false),error=ref('');
 let latest:T|undefined,timer:ReturnType<typeof setTimeout>|undefined,lastStart=-Infinity;
 function schedule(immediate=false){
  if(sending.value||latest===undefined)return;
  if(timer!==undefined)clearTimeout(timer);
  const delay=immediate?0:Math.max(0,100-(performance.now()-lastStart));
  if(!delay){void flush();return;}
  timer=setTimeout(()=>{timer=undefined;void flush();},delay);
 }
 async function flush(){
  if(sending.value||latest===undefined)return;
  const value=latest;latest=undefined;pending.value=false;sending.value=true;error.value='';lastStart=performance.now();
  try{await send(value);}catch(reason){error.value=reason instanceof Error?reason.message:String(reason);}
  finally{sending.value=false;schedule();}
 }
 function update(value:T|null,immediate=false){
  latest=value??undefined;pending.value=value!==null;
  if(value===null){if(timer!==undefined)clearTimeout(timer);timer=undefined;return;}
  schedule(immediate);
 }
 // Finish the user's last pending intent even if they immediately navigate away.
 onBeforeUnmount(()=>{if(timer!==undefined)clearTimeout(timer);timer=undefined;void flush();});
 return {sending,pending,error,update};
}

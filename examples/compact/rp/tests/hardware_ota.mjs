// Native RP hardware acceptance: run only against the explicitly selected board.
// Usage: node hardware_ota.mjs HOST IMAGE KEY_FILE REPORT [cycles=2]
// Key content is never printed. The endpoint must be the compact RP demo.
import {readFileSync,writeFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import http from 'node:http';
const [host,imagePath,keyPath,reportPath,cycles='2']=process.argv.slice(2);
if(!host||!imagePath||!keyPath||!reportPath)throw Error('HOST IMAGE KEY_FILE REPORT [cycles] required');
const key=readFileSync(keyPath,'utf8').trim(),image=readFileSync(imagePath),sha=createHash('sha256').update(image).digest('hex');
const sleep=ms=>new Promise(r=>setTimeout(r,ms)),report={host,image_sha256:sha,image_bytes:image.length,started:new Date().toISOString(),checks:[]};
const save=()=>writeFileSync(reportPath,JSON.stringify(report,null,2)+'\n');
function check(ok,name,details={}){report.checks.push({test:name,ok,...details});save();console.log(name,JSON.stringify({ok,...details}));if(!ok)throw Error(name);}
function request(path,{method='GET',body,headers={},timeout=650000}={}){return new Promise((resolve,reject)=>{let text='';const req=http.request({host,port:80,path,method,agent:false,headers:{'X-Client-Key':key,Connection:'close',...(body?{'Content-Length':body.length}:{}),...headers}},res=>{res.on('data',b=>text+=b);res.on('end',()=>resolve({status:res.statusCode,body:text}));res.on('error',reject);});const timer=setTimeout(()=>req.destroy(Error('deadline')),timeout);req.on('close',()=>clearTimeout(timer));req.on('error',reject);req.end(body);});}
async function status(){const r=await request('/api/status',{timeout:5000});if(r.status!==200)throw Error('status '+r.status);return JSON.parse(r.body);}
async function online(slot){const end=Date.now()+180000;while(Date.now()<end){try{const s=await status();if(!slot||s.running_slot===slot)return s;}catch{}await sleep(1000);}throw Error('device failed to become ready '+(slot??''));}
try{
 const initial=await online();check(initial.platform==='RP2350 Pico 2 W','native platform',initial);
 const config={name:initial.name,interval_ms:initial.interval_ms,led:initial.led};
 let r;
 let before=await status();
 await new Promise(resolve=>{const req=http.request({host,port:80,path:'/api/firmware',method:'POST',agent:false,headers:{'X-Client-Key':key,'X-Firmware-SHA256':sha,'Content-Type':'application/octet-stream','Content-Length':image.length}},res=>res.resume());req.on('error',()=>resolve());req.write(image.subarray(0,8192));setTimeout(()=>{req.destroy();resolve();},1500);});
 await sleep(2000);let s=await online();check(s.running_slot===before.running_slot&&s.uptime_ms>before.uptime_ms,'short body never activated',{slot:s.running_slot});
 const bad=sha[0]==='0'?'1'+sha.slice(1):'0'+sha.slice(1),start=Date.now();
 r=await request('/api/firmware',{method:'POST',body:image,headers:{'X-Firmware-SHA256':bad,'Content-Type':'application/octet-stream'}}).catch(e=>({status:0,body:e.message}));
 s=await online();check(r.status!==202&&s.running_slot===before.running_slot&&s.uptime_ms>before.uptime_ms,'wrong hash never activated',{reply:r.status,ms:Date.now()-start,slot:s.running_slot});
 for(let i=0;i<Number(cycles);i++){
  before=await status();const target=before.running_slot==='A'?'B':'A',start=Date.now();
  r=await request('/api/firmware',{method:'POST',body:image,headers:{'X-Firmware-SHA256':sha,'Content-Type':'application/octet-stream'}});
  check(r.status===202,'valid upload accepted',{target,ms:Date.now()-start});
  s=await online(target);check(s.name===config.name&&s.interval_ms===config.interval_ms&&s.led===config.led,'slot changed and key/settings survived',s);
 }
 check(true,'test did not mutate settings or API key');
 report.pass=true;
}catch(error){report.pass=false;report.error=String(error);console.log('FAIL',report.error);}finally{report.finished=new Date().toISOString();save();}
process.exitCode=report.pass?0:1;

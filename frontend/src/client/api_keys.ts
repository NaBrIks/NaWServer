import {DeviceClient,cborBody} from './index.ts';
export interface ApiKeySlot {slot:number;name:string;active:boolean;revision:number}
export interface ApiKeyList {slots:ApiKeySlot[];maximum_slots:number;ready?:boolean}
export class ApiKeyAdmin {
 private readonly client:DeviceClient;
 constructor(client:DeviceClient=new DeviceClient()){this.client=client;}
 private auth(user:string,password:string){return 'Basic '+btoa(user+':'+password);}
 async list(user:string,password:string):Promise<ApiKeyList>{
  const response=await this.client.request('/api/v2/keys',{headers:{Authorization:this.auth(user,password)}});
  if(!response.ok)throw Error('HTTP '+response.status);
  const data=response.data as ApiKeyList;
  if(data?.ready===false)throw Error('Key store unavailable');
  if(!data||!Number.isInteger(data.maximum_slots)||data.maximum_slots<1||data.maximum_slots>16||!Array.isArray(data.slots)||data.slots.length>data.maximum_slots)throw Error('Invalid key metadata');
  return data;
 }
 async change(action:'create'|'rotate'|'revoke',slot:number,name:string,user:string,password:string):Promise<{slot:number;secret?:string}>{
  if(action==='create'&&(new TextEncoder().encode(name).length>24||/[\u0000-\u001f\u007f]/.test(name)))throw Error('Key name must be at most 24 UTF-8 bytes without control characters');
  const body=cborBody({action,slot,name});
  const response=await this.client.request('/api/v2/keys',{method:'POST',...body,headers:{...body.headers,Authorization:this.auth(user,password)}});
  if(!response.ok)throw Error('HTTP '+response.status);
  const data=response.data as {slot:number;secret?:string};
  if(!data||!Number.isInteger(data.slot)||data.slot<0||data.slot>=16||(action!=='create'&&data.slot!==slot)||(action!=='revoke'&&(typeof data.secret!=='string'||!data.secret.length)))throw Error('Invalid key response');
  return data;
 }
}

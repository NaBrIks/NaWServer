import {pageId,pageHeaders,pageUrl} from './page_identity.ts';
export {pageId,pageHeaders,pageUrl} from './page_identity.ts';
// Framework-independent browser client. Importing this module starts no network
// activity and owns no UI state; callers control subscription lifetime.
import {decodeWire,diagnostic} from '../contracts/cbor.ts';
export * from '../contracts/index.ts';
export {encodeWire,decodeWire,cborBody,diagnostic} from '../contracts/cbor.ts';

export interface ClientOptions {baseUrl?:string;timeoutMs?:number;maximumResponseBytes?:number;fetch?:typeof fetch;pageIdentity?:boolean}
export interface ClientResponse {status:number;ok:boolean;data:unknown;text:string;bytes:number;challenge:string|null;retryAfter:string|null}
export class DeviceClient {
 private readonly baseUrl:string;
 private readonly timeoutMs:number;
 private readonly maximumResponseBytes:number;
 private readonly fetcher:typeof fetch;
 private readonly pageIdentity:boolean;
 constructor(options:ClientOptions={}){
  this.pageIdentity=options.pageIdentity??true;this.baseUrl=options.baseUrl??'';this.timeoutMs=options.timeoutMs??4000;
  this.maximumResponseBytes=options.maximumResponseBytes??65536;this.fetcher=options.fetch??globalThis.fetch.bind(globalThis);
 }
 private url(path:string){return this.baseUrl?new URL(path,this.baseUrl).href:path;}
 async request(path:string,init:RequestInit={}):Promise<ClientResponse>{
  const timeout=AbortSignal.timeout(this.timeoutMs);
  const signal=init.signal?AbortSignal.any([init.signal,timeout]):timeout;
  const response=await this.fetcher(this.url(path),{...init,headers:this.pageIdentity?pageHeaders(init.headers):init.headers,cache:'no-store',credentials:'omit',signal});
  const chunks:Uint8Array[]=[];let size=0;
  if(response.body){
   const reader=response.body.getReader();
   try{for(;;){const result=await reader.read();if(result.done)break;size+=result.value.length;
    if(size>this.maximumResponseBytes){await reader.cancel();throw Error('Response byte limit');}chunks.push(result.value);}}
   finally{reader.releaseLock();}
  }
  const raw=new Uint8Array(size);let offset=0;for(const chunk of chunks){raw.set(chunk,offset);offset+=chunk.length;}
  const isCbor=response.headers.get('content-type')?.split(';')[0]==='application/cbor';
  const data=isCbor&&raw.length?decodeWire(raw):undefined;
  return {status:response.status,ok:response.ok,data,text:isCbor?diagnostic(data):new TextDecoder().decode(raw),bytes:size,challenge:response.headers.get('www-authenticate'),retryAfter:response.headers.get('retry-after')};
 }
 openWebSocket(path='/ws'){
  const url=new URL(this.url(path),globalThis.location?.href??'http://localhost/');
  if(this.pageIdentity)url.searchParams.set('page_id',pageId());
  url.protocol=url.protocol==='https:'?'wss:':'ws:';
  const socket=new WebSocket(url);socket.binaryType='arraybuffer';return socket;
 }
 openEvents(path='/api/v2/events'){return new EventSource(this.pageIdentity?pageUrl(this.url(path)):this.url(path));}
}

export {SnapshotPoller,decodeObservers,observerStorageKey,type ObserverPreferences} from './observers.ts';

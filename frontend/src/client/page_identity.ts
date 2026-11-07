// One non-secret quota label per document. No storage survives reload or tab duplication.
let currentPageId:string|undefined;
export function createPageId():string{
 const bytes=new Uint8Array(16);globalThis.crypto.getRandomValues(bytes);
 return Array.from(bytes,value=>value.toString(16).padStart(2,'0')).join('');
}
export function pageId():string{return currentPageId??=createPageId();}
export function pageHeaders(input?:HeadersInit):Headers{
 const headers=new Headers(input);headers.set('X-Page-Id',pageId());return headers;
}
export function pageUrl(path:string,id:string=pageId()):string{
 const url=new URL(path,globalThis.location?.href??'http://localhost/');
 url.searchParams.set('page_id',id);
 return url.href;
}

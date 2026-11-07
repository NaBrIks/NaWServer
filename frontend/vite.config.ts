import {defineConfig} from 'vite';
import vue from '@vitejs/plugin-vue';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
import {existsSync} from 'node:fs';
const root=fileURLToPath(new URL('.',import.meta.url));
export function createFrontendConfig(mode:string){
 if(!/^[a-z0-9_]+$/.test(mode))throw Error('Invalid frontend profile '+mode);
 const projectRoot=resolve(root,'../examples',mode);
 if(!existsSync(resolve(projectRoot,'frontend/project.ts')))throw Error('Frontend profile does not exist: '+mode);
 const defaults:Record<string,string>={showcase:'http://127.0.0.1:8788',touch_lcd7:'http://127.0.0.1:8790'};
 const target=process.env.NAWSERVER_DEVICE_URL??(mode==='c6_lcd147'?process.env.NAWSERVER_C6_URL:defaults[mode]);
 return {root,plugins:[vue()],resolve:{dedupe:['vue','cborg'],alias:{'@frontend':resolve(root,'src'),'@project':resolve(projectRoot,'frontend/project.ts')}},build:{outDir:resolve(projectRoot,'frontend/dist'),emptyOutDir:true,target:'es2022',sourcemap:false,assetsInlineLimit:4096},server:{proxy:target?{'/mcp':target,'/api':target,'/sim':target,'/ws':{target:target.replace('http:','ws:'),ws:true}}:undefined}};
}
export default defineConfig(({mode})=>createFrontendConfig(mode));

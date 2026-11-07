import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
import {existsSync} from 'node:fs';
const root=fileURLToPath(new URL('../..',import.meta.url));
const selected=process.argv[2];
const targets=selected?[selected]:['showcase','touch_lcd7','c6_lcd147','rp2350_pico2_w'];
for(const target of targets){
 if(!/^[a-z0-9_]+$/.test(target)||!existsSync(resolve(root,'examples',target,'frontend/project.ts')))throw Error('Unknown frontend target '+target);
 for(const [cli,args] of [
  ['vue-tsc/bin/vue-tsc.js',['--noEmit','-p',resolve(root,'examples',target,'frontend/tsconfig.json')]],
  ['vite/bin/vite.js',['build','--config',resolve(root,'frontend/vite.config.ts'),'--mode',target]]
 ]){
  const result=spawnSync(process.execPath,[resolve(root,'node_modules',cli),...args],{cwd:root,stdio:'inherit'});
  if(result.error)throw result.error;
  if(result.status!==0)process.exit(result.status??1);
 }
}

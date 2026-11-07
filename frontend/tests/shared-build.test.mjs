import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFile,readdir,access} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
const root=fileURLToPath(new URL('../..',import.meta.url));
const read=path=>readFile(resolve(root,path),'utf8');
test('both demos consume the one shared source directory',async()=>{
 await access(resolve(root,'frontend/src/App.vue'));
 for(const target of ['showcase','touch_lcd7']){
  await assert.rejects(access(resolve(root,`examples/${target}/frontend/src`)));
  const config=JSON.parse(await read(`examples/${target}/frontend/tsconfig.json`));
  assert.equal(config.extends,'../../../frontend/tsconfig.json');
  assert(config.include.includes('../../../frontend/src/**/*.vue'));
 }
});
test('S3 LCD is a project extension, excluded from C3 output',async()=>{
 const s3=await readdir(resolve(root,'examples/touch_lcd7/frontend/dist/assets'));
 const c3=await readdir(resolve(root,'examples/showcase/frontend/dist/assets'));
 assert(s3.some(name=>/^BoardPanel-.*\.js$/.test(name)));
 assert(!c3.some(name=>/^BoardPanel-/.test(name)));
 const shared=await read('frontend/src/App.vue');
 assert(!shared.includes('BoardPanel'));
 assert(!shared.includes('GPIO6'));
 assert(shared.includes('project.pages'));
});
test('both targets emit the same common stylesheet',async()=>{
 const css=async target=>{
  const base=`examples/${target}/frontend/dist/assets`;
  const files=(await readdir(resolve(root,base))).filter(name=>/^index-.*\.css$/.test(name));
  assert.equal(files.length,1);
  return read(`${base}/${files[0]}`);
 };
 assert.equal(await css('showcase'),await css('touch_lcd7'));
});
test('project appearance and admission support stay isolated',async()=>{
 const c3=await read('examples/showcase/frontend/project.ts');
 const s3=await read('examples/touch_lcd7/frontend/project.ts');
 assert(c3.includes("storageKey:'nawserver.showcase.appearance'"));
 assert(s3.includes("storageKey:'nawserver.touch-lcd7.appearance'"));
 assert(c3.includes('admissionPreflight:false'));
 assert(s3.includes('admissionPreflight:true'));
 const state=await read('frontend/src/state/device.ts');
 assert(state.includes("project.admissionPreflight?await request('/api/v2/admission?kind=ws'"));
});
test('hardware labels come from actual state',async()=>{
 const rgb=await read('frontend/src/features/RgbPanel.vue');
 assert(rgb.includes('strip?.gpio'));
 assert(!rgb.includes('GPIO6'));
 const flow=await read('frontend/src/components/TransportDiagram.vue');
 assert(flow.includes('ui.info.chip.model'));
});

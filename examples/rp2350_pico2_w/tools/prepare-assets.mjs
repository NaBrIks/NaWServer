import {readFile, writeFile, mkdir,readdir} from 'node:fs/promises';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {gzipSync} from 'node:zlib';
const demo = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const run=spawnSync(process.execPath,[resolve(demo,'../../node_modules/vite/bin/vite.js'),'build','--config',resolve(demo,'frontend/vite.config.mjs')],{cwd:demo,stdio:'inherit'});
if(run.status!==0)throw Error('Shared Vue frontend build failed');
const dist=resolve(demo,'frontend/dist');
const files=await readdir(dist,{recursive:true});
const mimes={'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.woff':'font/woff','.svg':'image/svg+xml'};
let output = '#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace rp_demo {\n';
const entries=[];
let largest=0;
for (const file of files.filter(f=>/\.(html|js|css|woff|svg)$/.test(f)).sort()) {
 const path='/'+file.replaceAll('\\','/');
 const mime=mimes[path.slice(path.lastIndexOf('.'))];
 const bytes=gzipSync(await readFile(resolve(dist,file)),{level:9});
 // Flat owned filenames avoid coupling LittleFS directories to hashed routes.
 const storagePath=`/web.asset.${entries.length}.gz`;
 largest=Math.max(largest,bytes.length);
 const name=`resource_${entries.length}`;
 output+=`inline constexpr std::uint8_t ${name}[]={${[...bytes].join(',')}};\n`;
 entries.push(`{${JSON.stringify(path)},${JSON.stringify(storagePath)},${JSON.stringify(mime)},${name},sizeof(${name})}`);
}
// Assets stream through the configured TX blocks; firmware layout bounds their
// total immutable fallback storage, not a whole-file SRAM buffer.
output+='struct SeedAsset {const char *path;const char *storage_path;const char *mime;const std::uint8_t *bytes;std::size_t size;};\n';
output+=`inline constexpr SeedAsset seed_assets[]={${entries.join(',')}};\n}\n`;
await mkdir(resolve(demo,'build/generated'),{recursive:true});
await writeFile(resolve(demo,'build/generated/demo_assets.hpp'),output);
console.log(`Shared device frontend: ${entries.length} gzip assets, largest ${largest} bytes.`);

import {readFileSync,writeFileSync,mkdirSync} from 'node:fs';
import {gzipSync} from 'node:zlib';
import {dirname,resolve} from 'node:path';
const hash=readFileSync(new URL('sha256.mjs',import.meta.url),'utf8').replace('export async function','async function');
export const html=Buffer.from(readFileSync(new URL('index.html',import.meta.url),'utf8').replace('/*HASH*/',hash));const gzip=gzipSync(html,{level:9});
if(process.argv[2]){const path=resolve(process.argv[2]);mkdirSync(dirname(path),{recursive:true});writeFileSync(path,'#pragma once\n#include <cstddef>\nnamespace compact_web {\ninline constexpr unsigned char gzip[] = {'+Array.from(gzip).join(',')+'};\ninline constexpr std::size_t gzip_size = sizeof(gzip);\n}\n');}
console.log(JSON.stringify({html_bytes:html.length,gzip_bytes:gzip.length}));

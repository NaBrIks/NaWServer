import {readFileSync,mkdtempSync,unlinkSync,rmdirSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import assert from 'node:assert/strict';
const output=mkdtempSync(join(tmpdir(),'nawserver-http-profile-'));
try{
execFileSync('cmake',[`-DNAWSERVER_DEMO_PROFILE_TEST_OUTPUT=${output}`,'-P',fileURLToPath(new URL('ProfileTest.cmake',import.meta.url))]);
for(const [target,board] of Object.entries({esp32:'ESP32',esp32c3:'ESP32-C3',esp32c6:'ESP32-C6',esp32s3:'ESP32-S3'})){
 const profile=JSON.parse(readFileSync(join(output,target+'.json'),'utf8').replace(/^\uFEFF/,''));
 assert.equal(profile.backend,board);assert.equal(profile.scope,`Проверки выполняются на подключённой плате ${board}`);assert.equal(profile.transport,'HTTP/1.1 · LAN · shared TCP');
 if(target!=='esp32c3')assert.ok(!JSON.stringify(profile).includes('ESP32-C3'));
}
console.log('HTTP_TEST_TARGET_PROFILE_PASS targets=4');
}finally{for(const target of ['esp32','esp32c3','esp32c6','esp32s3']){try{unlinkSync(join(output,target+'.json'));}catch{}}rmdirSync(output);}

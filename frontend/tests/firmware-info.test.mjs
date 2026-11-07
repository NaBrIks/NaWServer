import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {isInfo,httpBackendName,jsonBackendName} from '../src/contracts/index.ts';

const info={v:2,source:'device',chip:{model:'ESP32-C3',cores:1},firmware:{version:'test',idf_version:'v6.0.3'},capabilities:{},limits:{frame_bytes:4096}};
test('actual backend metadata is optional for older demos and explicit when present',()=>{
 assert(isInfo(info));
 for(const backend of ['tcp','idf_httpd'])assert(isInfo({...info,firmware:{...info.firmware,http_backend:backend}}));
 for(const backend of ['httpd','unknown',null,4])assert(!isInfo({...info,firmware:{...info.firmware,http_backend:backend}}));
 assert.equal(httpBackendName('tcp'),'NaWServer TCP');
 assert.equal(httpBackendName('idf_httpd'),'ESP-IDF HTTPD');
 assert.equal(httpBackendName(undefined),'—');
});
test('detail and legacy identity views use firmware metadata without profile inference',async()=>{
 const details=await readFile(new URL('../src/features/DeviceDetails.vue',import.meta.url),'utf8');
 const fallback=await readFile(new URL('../src/features/ResourcesPanel.vue',import.meta.url),'utf8');
 for(const source of [details,fallback]){
  assert(source.includes('ui.info?.firmware.idf_version'));
  assert(source.includes('ui.info?.firmware.http_backend'));
  assert(source.includes('HTTP / WS / SSE'));
  assert(source.includes('httpBackendName'));
 }
 assert(details.includes('http_backend:ui.info?.firmware.http_backend??null'));
});

test('JSON build metadata is explicit and optional for older firmware',()=>{
 for(const json_backend of ['nawserver_bounded','yyjson','system_text_json','none'])assert(isInfo({...info,firmware:{...info.firmware,json_backend,mcp_enabled:true}}));
 for(const json_backend of [null,7,{}])assert(!isInfo({...info,firmware:{...info.firmware,json_backend}}));
 for(const mcp_enabled of ['true',1,null])assert(!isInfo({...info,firmware:{...info.firmware,mcp_enabled}}));
});

test('JSON backend labels use the actual firmware selection without guessing',()=>{
 assert.equal(jsonBackendName('nawserver_bounded'),'NaWServer bounded JSON');
 assert.equal(jsonBackendName('yyjson'),'yyjson');
 assert.equal(jsonBackendName('system_text_json'),'System.Text.Json');
 assert.equal(jsonBackendName('none'),'— (MCP off)');
 assert.equal(jsonBackendName(undefined),'—');
 assert.equal(jsonBackendName('future_backend'),'future_backend');
});

test('JSON/MCP byte budgets are optional nonnegative finite integers including disabled zero',()=>{
 assert(isInfo(info));
 for(const field of ['json_workspace_bytes','mcp_input_bytes','mcp_output_bytes']){
  for(const value of [0,1,2048,4096,16384])assert(isInfo({...info,firmware:{...info.firmware,[field]:value}}));
  for(const value of [-1,.5,NaN,Infinity,-Infinity,null,'4096',true,{},[]])assert(!isInfo({...info,firmware:{...info.firmware,[field]:value}}),`${field}: ${String(value)}`);
 }
 assert(isInfo({...info,firmware:{...info.firmware,mcp_enabled:false,json_workspace_bytes:0,mcp_input_bytes:0,mcp_output_bytes:0}}));
});
test('both INFO layouts show actual byte budgets with units delegated to formatting',async()=>{
 for(const name of ['DeviceDetails','ResourcesPanel']){
  const source=await readFile(new URL(`../src/features/${name}.vue`,import.meta.url),'utf8');
  for(const field of ['json_workspace_bytes','mcp_input_bytes','mcp_output_bytes'])assert(source.includes(`ui.info?.firmware.${field}`));
  for(const label of ['JSON: рабочая область','MCP: лимит запроса','MCP: буфер ответа'])assert(source.includes(label));
 }
});

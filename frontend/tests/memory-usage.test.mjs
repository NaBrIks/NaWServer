import {test} from 'node:test';
import assert from 'node:assert/strict';
import {memoryRegions,combinedMemory,isMemoryUsageRegion} from '../src/contracts/memory.ts';
const measured={id:'internal',total_bytes:1000,free_bytes:400,own_static_bytes:100,other_static_bytes:200,own_dynamic_bytes:50,other_dynamic_bytes:250};
test('runtime allocation segments exactly cover the supplied accessible region',()=>{
 assert(isMemoryUsageRegion(measured));
 const [region]=memoryRegions({memory:[],memory_usage:[measured]});
 assert.equal(region.measured,true);
 assert.equal(region.segments.reduce((sum,segment)=>sum+segment.bytes,0),region.total);
 assert.deepEqual(region.segments.map(segment=>segment.offset),[0,100,300,350,600]);
 assert.equal(combinedMemory([region,region]).total,2000);
});
test('old firmware exposes free and unclassified memory without inventing static ownership',()=>{
 const [region]=memoryRegions({memory:[{id:'internal',total_bytes:1000,free_bytes:400}]});
 assert.equal(region.measured,false);
 assert.deepEqual(region.segments.map(({kind,bytes})=>({kind,bytes})),[{kind:'free_bytes',bytes:400},{kind:'unknown_bytes',bytes:600}]);
 assert.deepEqual(memoryRegions(null),[]);
 assert.equal(combinedMemory([]).total,0);
});
test('invalid counts, inconsistent totals and unavailable zero pools are rejected',()=>{
 for(const delta of [{total_bytes:999},{own_dynamic_bytes:-1},{free_bytes:NaN},{own_static_bytes:1.5}])assert(!isMemoryUsageRegion({...measured,...delta}));
 assert.deepEqual(memoryRegions({memory:[],memory_usage:[{...measured,total_bytes:0,free_bytes:0,own_static_bytes:0,other_static_bytes:0,own_dynamic_bytes:0,other_dynamic_bytes:0}]}),[]);
});

import {test} from 'node:test';
import assert from 'node:assert/strict';
import {isStrip} from '../src/contracts/index.ts';

test('RGB telemetry accepts C6 one pixel and C3 ten pixels, rejects inconsistent counts',()=>{
 const strip={driver:'rmt',requested_driver:'rmt',driver_error:'',supported:true,enabled:true,gpio:8,count:1,mode:'solid',color:0,brightness_pct:20,period_ms:1000,pixels:[0]};
 assert(isStrip(strip));
 assert(isStrip({...strip,gpio:2,count:10,pixels:Array(10).fill(0)}));
 assert(!isStrip({...strip,count:10}));
 assert(!isStrip({...strip,count:0,pixels:[]}));
 assert(!isStrip({...strip,count:11,pixels:Array(11).fill(0)}));
});

import test from 'node:test';
import assert from 'node:assert/strict';
import {isTransmission,transmissionRate,occupiedTransmissionJobs} from '../src/contracts/transmission.ts';
import {isSummary} from '../src/contracts/index.ts';

const stats={enabled:true,maximum_jobs:8,maximum_bytes:16384,block_bytes:8192,maximum_jobs_per_connection:2,reserved_important_jobs:1,
 queued_jobs:1,filling_jobs:1,active_jobs:1,allocated_bytes:8192,used_bytes:4096,peak_bytes:8192,queued_bytes:1024,
 accepted_bytes:10000,sent_bytes:8000,discarded_bytes:0,failed_bytes:0,completed_jobs:3,discarded_jobs:0,rejected_jobs:0,failed_jobs:0,would_block_count:1};

test('TX contract accepts actual limits and exact counters but rejects malformed fields',()=>{
 assert.equal(isTransmission(stats),true);assert.equal(isTransmission({...stats,sent_bytes:18446744073709551615n}),true);
 for(const last_failure of [0,1,2,3,4])assert.equal(isTransmission({...stats,last_failure,last_native_error:-104}),true);
 for(const field of ['reserved_important_bytes','borrowed_bytes']){
  for(const value of [0,8192,18446744073709551615n])assert.equal(isTransmission({...stats,[field]:value}),true);
  for(const value of [-1,-1n,1.5,'8192'])assert.equal(isTransmission({...stats,[field]:value}),false);
 }
 for(const field of ['step_bytes','stall_timeout_ms','close_timeout_ms']){
  for(const value of [0,8192])assert.equal(isTransmission({...stats,[field]:value}),true);
  for(const value of [-1,1.5,'1000'])assert.equal(isTransmission({...stats,[field]:value}),false);
 }
 for(const extra of [{last_failure:5},{last_failure:-1},{last_failure:1.5},{last_native_error:1.5},{last_native_error:'104'}])assert.equal(isTransmission({...stats,...extra}),false);
 for(const [key,value] of [['used_bytes',-1],['maximum_jobs',1.5],['maximum_bytes',Infinity],['sent_bytes',-1n],['failed_jobs','1'],['enabled',1]])assert.equal(isTransmission({...stats,[key]:value}),false,key);
 const missing={...stats};delete missing.queued_bytes;assert.equal(isTransmission(missing),false);
});

test('TX rates use sample elapsed time and preserve reset/unsafe deltas as unavailable',()=>{
 assert.equal(transmissionRate(11264,10240,100),10240);assert.equal(transmissionRate(11264n,10240n,1000),1024);
 assert.equal(transmissionRate(1,100,1000),null);assert.equal(transmissionRate(1,0,0),null);
 assert.equal(transmissionRate(18446744073709551615n,0n,1000),null);
 assert.equal(occupiedTransmissionJobs({queued_jobs:3,filling_jobs:2,active_jobs:1}),5);
});

test('summary keeps old firmware compatible and validates TX when present',()=>{
 const old={uptime_ms:1000,sample_interval_ms:1000,cpu_busy_pct:10,core_busy_pct:[10],memory:[],rssi_dbm:null,task_count:1,ws_clients:1,network_status:'connected',
  counters:{http_requests:1,ws_rx_frames:1,ws_tx_frames:1,ws_rx_bytes:1,ws_tx_bytes:1,rejected:0},
  experiment:{running:false,kind:'pi',duration_ms:1,on_ms:1,period_ms:1,core:0,priority:1,elapsed_ms:0,progress_pct:0,pi:0,sample_ms:1,iterations:0,active:false},
  led:{supported:false,enabled:false,active_low:false,level:false,period_ms:1,duty_pct:0,gpio:0},
  strip:{driver:'rmt',requested_driver:'rmt',driver_error:'',supported:false,enabled:false,gpio:0,count:1,mode:'solid',color:0,brightness_pct:0,period_ms:1,pixels:[0]}};
 assert.equal(isSummary(old),true);assert.equal(isSummary({...old,transmission:stats}),true);
 assert.equal(isSummary({...old,transmission:{enabled:true}}),false);
});

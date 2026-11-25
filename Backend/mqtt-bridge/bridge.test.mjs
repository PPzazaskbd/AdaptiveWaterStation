import assert from 'node:assert/strict';
import {envelope,forward} from './forward.mjs';
const original={v:1,node_id:'test-node-01',boot_id:'console-test-01',seq:1,readings:{demo_value:42},test_data:true};
const data=Buffer.from(JSON.stringify(original));
assert.deepEqual(envelope(data),{schema_version:1,device_id:'test-node-01',measured_at:null,raw_payload:original});
assert.throws(()=>envelope(Buffer.from('{"message":"Hello from AWS IoT console"}')));
assert.throws(()=>envelope(Buffer.from('x'.repeat(12001))));
let posts=0;const seen=new Set();
const fetchImpl=async(url,options)=>{
  if(options?.method==='POST'){
    posts++;assert.deepEqual(JSON.parse(options.body).raw_payload,original);
    return new Response(JSON.stringify({stored:true,sample:{sample_id:'16'}}),{status:201});
  }
  return new Response(JSON.stringify({sample_id:'16',device_id:'test-node-01',received_at:'2026-11-20T00:00:00Z',raw_payload:original}),{status:200});
};
assert.equal((await forward(data,{fetchImpl,seen})).sample_id,'16');
assert.equal((await forward(data,{fetchImpl,seen})).skipped,true);assert.equal(posts,1);
const failureSeen=new Set();let attempts=0;
await assert.rejects(()=>forward(data,{seen:failureSeen,fetchImpl:async()=>{attempts++;throw new Error('ambiguous timeout');}}),/timeout/);
assert.equal((await forward(data,{seen:failureSeen,fetchImpl})).skipped,true);assert.equal(attempts,1);
console.log('PASS MQTT-to-HTTP mapping, original JSON preservation, exact-ID read-back, invalid input, session duplicate suppression, no ambiguous automatic retry.');
console.log('All requests simulated locally. No MQTT connection or cloud writes.');

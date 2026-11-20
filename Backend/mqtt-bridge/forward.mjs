const object=value=>value!==null&&typeof value==='object'&&!Array.isArray(value);
export function envelope(bytes){
  if(Buffer.byteLength(bytes)>12000)throw new Error('MQTT message exceeds demo limit');
  const message=JSON.parse(bytes.toString());
  if(!object(message)||message.v!==1||typeof message.node_id!=='string'||!message.node_id||message.node_id.length>128||!object(message.readings))throw new Error('Not a supported firmware v1 message');
  const measured=message.measured_at??null;
  if(measured!==null&&(typeof measured!=='string'||!/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,6})?(Z|[+-]\d{2}:\d{2})$/.test(measured)||!Number.isFinite(Date.parse(measured))))throw new Error('Invalid measured_at');
  return {schema_version:1,device_id:message.node_id,measured_at:measured,raw_payload:message};
}
export function identity(message){
  if(typeof message.boot_id!=='string'||!message.boot_id||!Number.isSafeInteger(message.seq)||message.seq<0)return null;
  return JSON.stringify([message.node_id,message.boot_id,message.seq]);
}
export async function forward(bytes,{apiBase='https://api.embed-project.thiraphatpp.com',fetchImpl=fetch,seen=new Set()}={}){
  const body=envelope(bytes),key=identity(body.raw_payload);
  if(key&&seen.has(key))return {skipped:true,reason:'already_seen_in_this_process'};
  const url=new URL(apiBase);
  if(url.protocol!=='https:'&&!(url.protocol==='http:'&&['localhost','127.0.0.1'].includes(url.hostname)))throw new Error('HTTPS required for remote API');
  if(url.pathname!=='/'||url.search||url.hash||url.username||url.password)throw new Error('api_base must be an origin');
  // Block automatic replay after an ambiguous POST. This is NOT durable idempotency.
  if(key){seen.add(key);if(seen.size>2000)seen.delete(seen.values().next().value);}
  const response=await fetchImpl(url.origin+'/api/telemetry',{
    method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body),signal:AbortSignal.timeout(15000)
  });
  const result=await response.json();
  if(response.status!==201||result.stored!==true||!result.sample?.sample_id)throw new Error(`API HTTP ${response.status}: ${result.error||'storage not confirmed'}`);
  const sample=result.sample;
  const read=await fetchImpl(url.origin+'/api/telemetry/'+encodeURIComponent(sample.sample_id),{signal:AbortSignal.timeout(15000)});
  const saved=await read.json();
  if(!read.ok||String(saved.sample_id)!==String(sample.sample_id)||saved.device_id!==body.device_id||JSON.stringify(saved.raw_payload)===undefined)throw new Error('Storage response received, but read-back failed; inspect sample '+sample.sample_id);
  if(saved.raw_payload.node_id!==body.device_id||saved.raw_payload.seq!==body.raw_payload.seq||saved.raw_payload.boot_id!==body.raw_payload.boot_id)throw new Error('Read-back identity mismatch for sample '+sample.sample_id);
  return {stored:true,sample_id:String(sample.sample_id),device_id:saved.device_id,received_at:saved.received_at};
}

import fs from 'node:fs';
import {forward} from './forward.mjs';
const configPath=process.argv[2]||'/etc/embedded-backend/iot/bridge.json';
const cfg=JSON.parse(fs.readFileSync(configPath,'utf8'));
if(!/^[a-z0-9-]+-ats\.iot\.ap-southeast-2\.amazonaws\.com$/.test(cfg.endpoint))throw new Error('Expected Sydney AWS IoT Data-ATS endpoint, without protocol or path');
const topic='water/demo/telemetry';
const {default:mqtt}=await import('mqtt');
const client=mqtt.connect({
  protocol:'mqtts',host:cfg.endpoint,port:8883,clientId:'ec2-cloud-bridge',protocolVersion:4,
  cert:fs.readFileSync('/etc/embedded-backend/iot/bridge.crt'),
  key:fs.readFileSync('/etc/embedded-backend/iot/bridge.key'),
  ca:fs.readFileSync('/etc/embedded-backend/iot/AmazonRootCA1.pem'),
  rejectUnauthorized:true,clean:true,reconnectPeriod:5000,connectTimeout:10000,keepalive:30
});
let subscribed=false,working=false,stopping=false;
const queue=[],seen=new Set();
client.on('connect',()=>{
  client.subscribe(topic,{qos:1},(error,granted)=>{
    if(error||!granted?.length||granted.some(item=>item.qos===128)){
      console.error('[bridge] Subscribe failed; check certificate policy.');return;
    }
    subscribed=true;console.log('[bridge] SUBSCRIBED '+topic+'; waiting for messages');
  });
});
client.on('error',error=>console.error('[bridge] MQTT error:',error.message));
client.on('offline',()=>{subscribed=false;console.error('[bridge] MQTT offline; reconnecting');});
client.on('message',(received,payload)=>{
  if(received!==topic||stopping)return;
  if(payload.length>12000){console.error('[bridge] Rejected oversized message');return;}
  if(queue.length>=100){console.error('[bridge] Queue full; message not forwarded. Demo is not a durable queue.');return;}
  queue.push(Buffer.from(payload));drain();
});
async function drain(){
  if(working)return;working=true;
  try{
    while(queue.length){
      const bytes=queue.shift();
      try{
        const result=await forward(bytes,{apiBase:cfg.api_base||'https://api.embed-project.thiraphatpp.com',seen});
        if(result.skipped)console.log('[bridge] SKIP seen identity (session-only cache)');
        else console.log(`[bridge] DB SAVED + READ BACK device=${result.device_id} sample=${result.sample_id} received=${result.received_at}`);
      }catch(error){console.error('[bridge] NOT CONFIRMED:',error.message,'No automatic HTTP retry.');}
    }
  }finally{working=false;}
}
const startup=setTimeout(()=>{if(!subscribed){console.error('[bridge] No successful subscription after 30 seconds. Check endpoint, certificate Active status and attached policy.');client.end(true);process.exitCode=1;}},30000);
startup.unref();
function stop(){if(stopping)return;stopping=true;console.log('[bridge] Stopping. Remaining RAM queue is not durable.');client.end(false,()=>process.exit(0));setTimeout(()=>process.exit(0),3000).unref();}
process.once('SIGINT',stop);process.once('SIGTERM',stop);

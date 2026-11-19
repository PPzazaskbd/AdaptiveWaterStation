// Connection.cpp - cloudBegin and cloudLoop: connecting, the heartbeat, and publishing the buffer
// (communication-plan.md activity diagram 4.5).

#include <esp_system.h>
#include <sys/time.h>

#include <new>

#include "Format.h"
#include "Station.h"

using namespace cloud_detail;

namespace {

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

long long epochMsNow() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Publishes the oldest record. It leaves the buffer only on AWS's PUBACK.
void publishOne() {
  Station &s = st();
  CloudRecord *r = s.queue.front();
  if (!r || !awsIotClockValid()) return;

  // measured_at = when the sample was taken, even if that was before the clock was set.
  char at[32];
  cloudIso(epochMsNow() - (long long)(uint32_t)(millis() - r->monoMs), at, sizeof at);
  uint32_t dropped = r->board == CLOUD_GATEWAY ? s.queue.dropped() : 0;
  CloudFormatInfo info{idOf(r->board), s.bootId, at, (uint16_t)(s.queue.count() - 1), dropped, resetReason()};
  int n = cloudFormat(*r, info, s.json, sizeof s.json);
  if (n < 0) {  // can't happen with 1 KB; never block the buffer on it
    if (verbose()) Serial.printf("[cloud] %s seq %lu too large, dropped\n", info.nodeId, (unsigned long)r->seq);
    s.queue.popFront();
    return;
  }

  uint32_t t0 = millis();
  if (awsIotPublish(s.cfg.topic, s.json)) {
    if (verbose())
      Serial.printf("[cloud] %s seq %lu delivered (PUBACK in %lu ms, %u waiting)%s\n", info.nodeId,
                    (unsigned long)r->seq, (unsigned long)(millis() - t0), (unsigned)info.queueDepth,
                    r->event.type ? " with an event" : "");
    s.queue.forgetDropped(dropped);
    s.queue.popFront();
  } else {
    if (r->attempt < 32767) r->attempt++;
    s.retryWait = true;  // wait 1 s: don't spin on a dead connection
    s.retryFrom = millis();
    if (verbose()) Serial.printf("[cloud] %s seq %lu not confirmed, will resend\n", info.nodeId, (unsigned long)r->seq);
  }
}

void startAws() {
  Station &s = st();
  // Retries come from cloudLoop: wait at most 2 s for the clock each time, not 15 s, so a network that
  // blocks NTP doesn't freeze loop() for 15 of every 20 s. SNTP keeps trying in the background anyway.
  AwsIotConfig aws = s.cfg.aws;
  if (s.lastBeginTry) aws.clockTimeoutMs = 2000;
  s.lastBeginTry = millis() | 1;  // never 0: 0 means "first try"
  AwsIotBeginResult r = awsIotBegin(aws);
  if (r == AWSIOT_OK || r == AWSIOT_NOT_YET) {
    s.awsStarted = true;
    WiFi.setSleep(false);  // in case the sketch connected Wi-Fi itself before cloudBegin
    WiFi.setAutoReconnect(true);
  } else if (verbose()) {
    Serial.printf("[cloud] start failed (%d: %s), retrying in 5 s\n", (int)r,
                  r == AWSIOT_NO_WIFI ? "no Wi-Fi" : r == AWSIOT_NO_CLOCK ? "no clock" : "bad config");
  }
}

}  // namespace

bool cloudBegin(const CloudConfig &cfg) {
  Station &s = st();
  if (s.began) return true;
  if (!cfg.aws.thingName || !cfg.nodeId || !cfg.topic || cfg.bufferRecords == 0) return false;
  s.cfg = cfg;
  s.slots = new (std::nothrow) CloudRecord[cfg.bufferRecords];
  if (!s.slots) {
    if (verbose()) Serial.println("[cloud] not enough memory for the buffer: lower bufferRecords");
    return false;
  }
  s.queue.init(s.slots, cfg.bufferRecords);
  s.began = true;
  startAws();
  // boot_id only after Wi-Fi has started: esp_random() is truly random only while the radio is on.
  // Before that it may repeat after a reboot, and the cloud would then drop the new boot's samples as
  // duplicates of the old ones (same node_id, boot_id, seq).
  snprintf(s.bootId, sizeof s.bootId, "%08lx", (unsigned long)esp_random());
  if (verbose()) {
    Serial.printf("[cloud] gateway \"%s\", node \"%s\", boot_id %s, topic %s, buffer %u records\n",
                  cfg.aws.thingName, cfg.nodeId, s.bootId, cfg.topic, (unsigned)cfg.bufferRecords);
    if (strcmp(cfg.topic, "water/demo/telemetry") == 0 &&
        (strcmp(cfg.aws.thingName, "gw-01") != 0 || strcmp(cfg.nodeId, "node-01") != 0))
      Serial.println("[cloud] warning: the demo stores only gw-01 and node-01; other ids are dropped");
  }
  // The first heartbeat (seq 0, with reset_reason) goes out as soon as the connection is up.
  record(CLOUD_GATEWAY, nullptr);
  return true;
}

bool cloudBegin(const char *thingName, const char *wifiSsid, const char *wifiPassword, const char *endpoint,
                const char *rootCa, const char *deviceCert, const char *privateKey) {
  CloudConfig cfg;
  cfg.aws.thingName = thingName;
  cfg.aws.wifiSsid = wifiSsid;
  cfg.aws.wifiPassword = wifiPassword;
  cfg.aws.endpoint = endpoint;
  cfg.aws.rootCa = rootCa;
  cfg.aws.deviceCert = deviceCert;
  cfg.aws.privateKey = privateKey;
  return cloudBegin(cfg);
}

void cloudLoop() {
  Station &s = st();
  if (!s.began) return;
  if (!s.awsStarted) {
    // Retry only once Wi-Fi is back (it reconnects by itself), so loop() isn't blocked by Wi-Fi waits.
    if (millis() - s.lastBeginTry >= 5000 && WiFi.status() == WL_CONNECTED) startAws();
  } else {
    awsIotLoop();
  }

  bool up = s.awsStarted && awsIotConnected();
  if (up && !s.wasUp) {
    // Just (re)connected: a FRESH heartbeat goes first, so the station shows online at once. An old
    // heartbeat from the backlog doesn't count: the cloud keeps last_seen_at at its newest value, so an
    // old one would leave the station OFFLINE until the whole backlog was sent.
    CloudRecord *f = s.queue.front();
    bool freshFirst = f && f->board == CLOUD_GATEWAY && f->event.type == CLOUD_EV_NONE &&
                      millis() - f->monoMs < 5000;
    if (!freshFirst) record(CLOUD_GATEWAY, nullptr, true);
  }
  s.wasUp = up;

  // Read the clock only now: the connection step above can block for seconds, and a `now` from before
  // it is older than lastHeartbeat, which made the subtraction wrap and sent a second heartbeat at once.
  checkSession();
  uint32_t now = millis();
  if (now - s.lastHeartbeat >= s.cfg.heartbeatMs) record(CLOUD_GATEWAY, nullptr);
  if (s.nodeActive && s.cfg.nodeSampleMs && now - s.lastNodeSample >= s.cfg.nodeSampleMs) record(CLOUD_NODE, nullptr);

  if (s.retryWait && millis() - s.retryFrom >= 1000) s.retryWait = false;
  if (up && s.queue.count() && !s.retryWait) publishOne();
}

bool cloudConnected() { return st().awsStarted && awsIotConnected(); }
uint16_t cloudBuffered() { return st().queue.count(); }
uint32_t cloudDropped() { return st().queue.dropped(); }

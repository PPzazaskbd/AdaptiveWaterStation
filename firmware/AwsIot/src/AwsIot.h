// AwsIot.h - connect an ESP32 to AWS IoT Core and publish JSON.
//
// Adaptive Water Station, group 67. Plan: SystemArchitecture/communication-plan.md section 1.1.
// Built on the same pieces as AWS's tutorial "Building an AWS IoT Core device using AWS Serverless and an
// ESP32" (WiFiClientSecure + the "MQTT" library by Joel Gaehwiler, secrets.h with THINGNAME and AWS_CERT_*),
// with what our gateway needs on top:
//  * QoS 1: awsIotPublish() returns true only after AWS answers with a PUBACK, so you know it arrived.
//    (The tutorial publishes at QoS 0, fire and forget. PubSubClient can't do QoS 1 at all.)
//  * The clock is set by SNTP before connecting, for measured_at (the cloud rejects > 5 min in the future).
//  * No endless while-loops: connect attempts time out, and awsIotLoop() reconnects Wi-Fi and MQTT
//    in the background every 5 s, subscribing again each time.
//
// Needs: Arduino-ESP32 core (2.x or 3.x) and the library "MQTT" by Joel Gaehwiler (Library Manager).
//
// Typical use (see examples/HelloDemo):
//
//   #include "secrets.h"                     // THINGNAME, WIFI_*, AWS_IOT_ENDPOINT, AWS_CERT_* (never commit)
//   #include <AwsIot.h>
//
//   void setup() {
//     Serial.begin(115200);
//     awsIotBegin(THINGNAME, WIFI_SSID, WIFI_PASSWORD, AWS_IOT_ENDPOINT,
//                 AWS_CERT_CA, AWS_CERT_CRT, AWS_CERT_PRIVATE);
//   }
//   void loop() {
//     awsIotLoop();                          // call often: keeps the connection alive, runs callbacks
//     if (awsIotConnected()) awsIotPublish("water/demo/telemetry", json);   // true = AWS has it
//   }
//
// Things that look like bugs but are AWS rules:
//  * The client id MUST equal the IoT thing name (e.g. "gw-01"). The policy uses ${iot:Connection.Thing.ThingName}.
//  * Publishing or subscribing to a topic the policy does not allow makes AWS CLOSE the connection.
//    It shows up as connect -> disconnect -> connect forever, not as an error. Check the topic and the policy.
//  * Only one connection per client id. A second board or a desktop MQTT tool using "gw-01" knocks the first
//    one off, and the two keep kicking each other. Never test as gw-01 while the gateway is running.
//
// Timing: awsIotPublish() at QoS 1 waits for the PUBACK, usually 50-300 ms, at most ackTimeoutMs (1 s).
// A reconnect (TLS handshake) blocks for 1-3 s. While blocked, the UART from the node keeps filling its
// receive buffer, so give it room: Serial2.setRxBufferSize(4096) before Serial2.begin().
//
// Header-only: every function is `inline`, so it can be included from several .cpp files.

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <sys/time.h>
#include <time.h>

// ---------------------------------------------------------------- settings

struct AwsIotConfig {
  const char *thingName = nullptr;    // = MQTT client id, e.g. "gw-01"
  const char *wifiSsid = nullptr;
  const char *wifiPassword = nullptr;
  const char *endpoint = nullptr;     // "xxxxxxxx-ats.iot.ap-southeast-2.amazonaws.com"
  const char *rootCa = nullptr;       // Amazon Root CA 1, PEM text
  const char *deviceCert = nullptr;   // this thing's certificate, PEM text
  const char *privateKey = nullptr;   // this thing's private key, PEM text

  // false = clean session (the default). true = persistent session: AWS keeps this client's QoS 1
  // subscriptions and queued command messages for 1 h while it is offline. Open decision 5 in the plan.
  bool persistentSession = false;
  int keepAliveSec = 60;
  int bufferSize = 2048;              // largest message in or out, bytes. Our samples are < 1 KB
  int ackTimeoutMs = 1000;            // how long publish/subscribe wait for AWS's answer
  uint32_t wifiTimeoutMs = 20000;     // awsIotBegin gives up waiting for Wi-Fi after this
  uint32_t clockTimeoutMs = 15000;    // ... and for the SNTP clock after this
  uint32_t retryEveryMs = 5000;       // awsIotLoop reconnects at most this often
  bool verbose = true;                // print what happens on Serial
};

enum AwsIotBeginResult {
  AWSIOT_OK = 0,        // connected to AWS
  AWSIOT_NOT_YET,       // Wi-Fi and clock fine, MQTT not connected yet; awsIotLoop() keeps trying
  AWSIOT_BAD_CONFIG,    // a required setting is missing
  AWSIOT_NO_WIFI,       // Wi-Fi did not connect in time (wrong password? 5 GHz? ESP32 is 2.4 GHz only)
  AWSIOT_NO_CLOCK,      // SNTP did not set the clock in time (network blocks UDP 123?)
};

// Incoming message on a subscribed topic. Runs inside awsIotLoop(). Don't publish or subscribe from
// inside it (the MQTT library forbids that): copy what you need and act on it in loop().
typedef void (*AwsIotMessageFn)(const char *topic, const char *payload, int len);
typedef void (*AwsIotConnectionFn)(bool connected);

// ---------------------------------------------------------------- internals

namespace awsiot_detail {

constexpr int kMaxSubs = 4;
constexpr time_t kClockValidAfter = 1767225600;  // 2026-01-01T00:00:00Z: earlier means SNTP has not run

struct State {
  AwsIotConfig cfg;
  WiFiClientSecure net;
  MQTTClient *mqtt = nullptr;  // created in awsIotBegin with cfg.bufferSize
  char subs[kMaxSubs][128] = {{0}};
  int subQos[kMaxSubs] = {0};
  AwsIotMessageFn onMessage = nullptr;
  AwsIotConnectionFn onConnection = nullptr;
  bool wasConnected = false;
  uint32_t lastTry = 0;
  uint32_t connects = 0;
};

inline State &st() {
  static State s;
  return s;
}

inline void onMqttMessage(MQTTClient *, char topic[], char bytes[], int length) {
  State &s = st();
  if (!s.onMessage) return;
  // The library's buffer is not NUL-terminated; copy so the callback gets a C string.
  char *p = (char *)malloc((size_t)length + 1);
  if (!p) return;
  memcpy(p, bytes, (size_t)length);
  p[length] = '\0';
  s.onMessage(topic, p, length);
  free(p);
}

inline void explainFailure() {
  State &s = st();
  if (!s.cfg.verbose) return;
  lwmqtt_err_t err = s.mqtt->lastError();
  lwmqtt_return_code_t rc = s.mqtt->returnCode();
  char tls[96] = "";
  int tlsErr = s.net.lastError(tls, sizeof tls);
  if (tlsErr)
    Serial.printf("[aws] MQTT: TLS failed (%d: %s). Wrong endpoint, certificate or key, or the certificate "
                  "is inactive or not attached to the thing\n", tlsErr, tls);
  else if (rc != LWMQTT_CONNECTION_ACCEPTED && err == LWMQTT_CONNECTION_DENIED)
    Serial.printf("[aws] MQTT: refused by AWS, CONNACK code %d\n", (int)rc);
  else
    Serial.printf("[aws] MQTT: connect failed (lwmqtt error %d)\n", (int)err);
}

// One connect attempt. Blocks for the TLS handshake (1-3 s).
inline bool tryConnect() {
  State &s = st();
  if (WiFi.status() != WL_CONNECTED) return false;
  if (s.cfg.verbose) Serial.printf("[aws] MQTT: connecting to %s as \"%s\"\n", s.cfg.endpoint, s.cfg.thingName);
  if (!s.mqtt->connect(s.cfg.thingName)) {
    explainFailure();
    s.net.stop();
    return false;
  }
  s.connects++;
  if (s.cfg.verbose) Serial.printf("[aws] MQTT: connected (#%lu)\n", (unsigned long)s.connects);
  if (s.connects > 3 && s.cfg.verbose)
    Serial.println("[aws] many reconnects: topic not allowed by the policy, or another client using this id?");
  // With a clean session AWS forgets subscriptions, so subscribe again on every connect.
  for (int i = 0; i < kMaxSubs; i++)
    if (s.subs[i][0] && !s.mqtt->subscribe(s.subs[i], s.subQos[i]) && s.cfg.verbose)
      Serial.printf("[aws] subscribe to %s failed\n", s.subs[i]);
  return true;
}

}  // namespace awsiot_detail

// ---------------------------------------------------------------- public API

inline void awsIotOnMessage(AwsIotMessageFn fn) { awsiot_detail::st().onMessage = fn; }
inline void awsIotOnConnection(AwsIotConnectionFn fn) { awsiot_detail::st().onConnection = fn; }

// True once SNTP has set the clock. Needed for measured_at.
inline bool awsIotClockValid() { return time(nullptr) > awsiot_detail::kClockValidAfter; }

// Writes the current UTC time as "2026-11-15T18:02:12.071Z" (the measured_at format). Needs 25 bytes.
// Returns false (and writes "") if the clock is not set yet.
inline bool awsIotNowIso(char *buf, size_t len) {
  if (!buf || len < 25) return false;
  buf[0] = '\0';
  if (!awsIotClockValid()) return false;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm t;
  gmtime_r(&tv.tv_sec, &t);
  size_t n = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &t);
  snprintf(buf + n, len - n, ".%03dZ", (int)(tv.tv_usec / 1000));
  return true;
}

// Wi-Fi, then the clock (SNTP), then one MQTT connect attempt. Blocks for a few seconds.
// Even if MQTT doesn't connect yet (AWSIOT_NOT_YET), awsIotLoop() keeps retrying.
// The strings must stay valid while the program runs (the secrets.h constants do).
inline AwsIotBeginResult awsIotBegin(const AwsIotConfig &cfg) {
  using namespace awsiot_detail;
  State &s = st();
  if (!cfg.thingName || !cfg.wifiSsid || !cfg.endpoint || !cfg.rootCa || !cfg.deviceCert || !cfg.privateKey)
    return AWSIOT_BAD_CONFIG;
  s.cfg = cfg;

  // 1. Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem sleep makes ESP32-CAM connections drop under load
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);
    if (cfg.verbose) Serial.printf("[aws] Wi-Fi: connecting to \"%s\"", cfg.wifiSsid);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - t0 > cfg.wifiTimeoutMs) {
        if (cfg.verbose) Serial.println(" timed out");
        return AWSIOT_NO_WIFI;
      }
      delay(250);
      if (cfg.verbose) Serial.print('.');
    }
    if (cfg.verbose) Serial.printf(" ok, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }

  // 2. Clock
  if (!awsIotClockValid()) {
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    uint32_t t0 = millis();
    while (!awsIotClockValid()) {
      if (millis() - t0 > cfg.clockTimeoutMs) {
        if (cfg.verbose) Serial.println("[aws] clock: SNTP timed out");
        return AWSIOT_NO_CLOCK;
      }
      delay(200);
    }
  }
  if (cfg.verbose) {
    char now[32];
    awsIotNowIso(now, sizeof now);
    Serial.printf("[aws] clock: %s\n", now);
  }

  // 3. MQTT over TLS, port 8883 (same calls as the AWS tutorial)
  s.net.setCACert(cfg.rootCa);
  s.net.setCertificate(cfg.deviceCert);
  s.net.setPrivateKey(cfg.privateKey);
  if (!s.mqtt) s.mqtt = new MQTTClient(cfg.bufferSize);
  s.mqtt->begin(cfg.endpoint, 8883, s.net);
  s.mqtt->setKeepAlive(cfg.keepAliveSec);
  s.mqtt->setCleanSession(!cfg.persistentSession);
  s.mqtt->setTimeout(cfg.ackTimeoutMs);
  s.mqtt->onMessageAdvanced(onMqttMessage);

  s.lastTry = millis();
  if (!tryConnect()) return AWSIOT_NOT_YET;
  s.wasConnected = true;
  if (s.onConnection) s.onConnection(true);
  return AWSIOT_OK;
}

// Same, with the secrets.h names from the AWS tutorial and default settings.
inline AwsIotBeginResult awsIotBegin(const char *thingName, const char *wifiSsid, const char *wifiPassword,
                                     const char *endpoint, const char *rootCa, const char *deviceCert,
                                     const char *privateKey) {
  AwsIotConfig cfg;
  cfg.thingName = thingName;
  cfg.wifiSsid = wifiSsid;
  cfg.wifiPassword = wifiPassword;
  cfg.endpoint = endpoint;
  cfg.rootCa = rootCa;
  cfg.deviceCert = deviceCert;
  cfg.privateKey = privateKey;
  return awsIotBegin(cfg);
}

// True while the MQTT connection to AWS is up.
inline bool awsIotConnected() {
  awsiot_detail::State &s = awsiot_detail::st();
  return s.mqtt && s.mqtt->connected();
}

// Call from loop() as often as you can. Receives messages, sends keep-alives, and reconnects
// (every retryEveryMs) after Wi-Fi or MQTT drops.
inline void awsIotLoop() {
  using namespace awsiot_detail;
  State &s = st();
  if (!s.mqtt) return;
  s.mqtt->loop();
  bool up = s.mqtt->connected();
  if (up != s.wasConnected) {
    s.wasConnected = up;
    if (!up && s.cfg.verbose) Serial.println("[aws] MQTT: disconnected, will retry");
    if (s.onConnection) s.onConnection(up);
  }
  if (up || millis() - s.lastTry < s.cfg.retryEveryMs) return;
  s.lastTry = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (s.cfg.verbose) Serial.println("[aws] Wi-Fi: down, waiting for it to reconnect");
    return;  // setAutoReconnect handles Wi-Fi
  }
  if (tryConnect()) {
    s.wasConnected = true;
    if (s.onConnection) s.onConnection(true);
  }
}

// Publishes a NUL-terminated payload (usually JSON).
//   QoS 1 (default): returns true only after AWS's PUBACK, so the message is in the cloud.
//                    false = not connected, or no PUBACK within ackTimeoutMs: keep it and send it again
//                    later (the gateway buffer in plan section 4.5). A resend may arrive twice; the
//                    cloud drops duplicates by (node_id, boot_id, seq).
//   QoS 0:           true = handed to the network, no confirmation.
inline bool awsIotPublish(const char *topic, const char *payload, int qos = 1) {
  awsiot_detail::State &s = awsiot_detail::st();
  if (!s.mqtt || !s.mqtt->connected() || !topic || !payload) return false;
  return s.mqtt->publish(topic, payload, (int)strlen(payload), false, qos);
}

// Subscribes to a topic (at most 4), now if connected and again after every reconnect.
// Messages arrive in the awsIotOnMessage callback. The policy must allow iot:Subscribe on the
// topic filter and iot:Receive on the topic, or AWS closes the connection.
inline bool awsIotSubscribe(const char *topic, int qos = 1) {
  using namespace awsiot_detail;
  State &s = st();
  if (!topic || strlen(topic) >= sizeof s.subs[0]) return false;
  for (int i = 0; i < kMaxSubs; i++) {
    bool mine = s.subs[i][0] && strcmp(s.subs[i], topic) == 0;
    if (!mine && s.subs[i][0]) continue;
    if (!mine) {
      strcpy(s.subs[i], topic);
      s.subQos[i] = qos;
    }
    return !awsIotConnected() || s.mqtt->subscribe(topic, qos);
  }
  return false;
}

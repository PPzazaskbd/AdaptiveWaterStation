// HelloDemo: build-order step 1 (communication-plan.md section 1.3).
// The gateway alone: Wi-Fi -> SNTP -> MQTT over TLS -> publish its own sample every 2 s to the demo topic,
// so it shows on the demo page. No node, no sensors: readings are empty.
//
// Before uploading: copy secrets.example.h to secrets.h and fill it in.
// Needs the library "MQTT" by Joel Gaehwiler (Library Manager).
// Board: "AI Thinker ESP32-CAM" (or "ESP32 Dev Module"). Serial Monitor at 115200.

#include "secrets.h"
#include <AwsIot.h>

static const char *TOPIC = "water/demo/telemetry";  // the demo's topic; later water/<thing>/telemetry
static const uint32_t PUBLISH_EVERY_MS = 2000;      // 2 s for the video (trap 3); the real gateway uses 15 s

static char bootId[9];  // new random id each boot, so the cloud can tell reboots apart
static uint32_t seq = 0;
static uint32_t attempt = 1;  // 2, 3, ... when the same seq is sent again
static uint32_t lastPublish = 0;

void onConnection(bool connected) {
  Serial.println(connected ? "cloud: up" : "cloud: down");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\nHelloDemo, thing \"%s\"\n", THINGNAME);

  awsIotOnConnection(onConnection);
  AwsIotBeginResult r;
  while ((r = awsIotBegin(THINGNAME, WIFI_SSID, WIFI_PASSWORD, AWS_IOT_ENDPOINT,
                          AWS_CERT_CA, AWS_CERT_CRT, AWS_CERT_PRIVATE)) > AWSIOT_NOT_YET) {
    if (r == AWSIOT_BAD_CONFIG) {
      Serial.println("secrets.h is incomplete: fix it and re-upload");
      while (true) delay(1000);
    }
    Serial.printf("awsIotBegin failed (%d), retrying in 5 s\n", (int)r);
    delay(5000);
  }
  // AWSIOT_NOT_YET is fine: awsIotLoop() keeps retrying the MQTT connection.

  // boot_id after Wi-Fi is up: esp_random() is truly random only while the radio is on. Before that it
  // may repeat after a reboot, and the cloud would drop the new boot's samples as duplicates.
  snprintf(bootId, sizeof bootId, "%08lx", (unsigned long)esp_random());
  Serial.printf("boot_id %s\n", bootId);
}

void loop() {
  awsIotLoop();

  if (millis() - lastPublish < PUBLISH_EVERY_MS || !awsIotConnected()) return;
  lastPublish = millis();

  char at[32];
  if (!awsIotNowIso(at, sizeof at)) return;

  // Gateway sample, message-formats.md section 6. v must be 1 or the demo drops it (trap 1).
  char json[384];
  snprintf(json, sizeof json,
           "{\"v\":1,\"node_id\":\"%s\",\"boot_id\":\"%s\",\"seq\":%lu,\"sample_mono_ms\":%lu,\"attempt\":%lu,"
           "\"measured_at\":\"%s\",\"readings\":{},"
           "\"diag\":{\"faults\":[],\"queue_depth\":0,\"fw_state\":\"LIVE\",\"buffer\":0}}",
           THINGNAME, bootId, (unsigned long)seq, (unsigned long)millis(), (unsigned long)attempt, at);

  uint32_t t0 = millis();
  if (awsIotPublish(TOPIC, json)) {
    Serial.printf("seq %lu delivered (PUBACK in %lu ms): %s\n", (unsigned long)seq,
                  (unsigned long)(millis() - t0), json);
    seq++;  // next seq only after AWS confirmed this one
    attempt = 1;
  } else {
    Serial.printf("seq %lu not confirmed, will send again\n", (unsigned long)seq);
    attempt++;
  }
}

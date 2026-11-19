// StationDemo: the station's sensors to AWS IoT Core, with no JSON in the sketch.
// Same pins as firmware/s3MainSensorCode: IR obstacle sensor on GPIO4, DHT11 on GPIO32,
// ESP32-CAM on Serial2 (RX 16, TX 17), answering 'P' with "PONG".
//
// What reaches the cloud (demo page, topic water/demo/telemetry):
//   gw-01   every 2 s: temperature and humidity (the heartbeat that keeps the station ONLINE)
//   node-01 a bottle stays 1 s in front of the IR sensor -> PRESENCE, state DISPENSING
//           the bottle leaves                            -> SUMMARY BOTTLE_REMOVED, state STANDBY
//   gw-01   the camera misses 2 pings in a row           -> FAULT CAM_FAULT (FAULT_CLEARED after 2 good ones)
//
// Before uploading: copy secrets.example.h to secrets.h and fill it in (never commit secrets.h).
// Libraries: "MQTT" by Joel Gaehwiler, "DHT sensor library" by Adafruit, and the folders
// firmware/AwsIot and firmware/StationCloud copied into Documents/Arduino/libraries.
// Board: "ESP32 Dev Module". Serial Monitor at 115200.

#include <DHT.h>

#include "secrets.h"
#include <StationCloud.h>

const int PIN_IR = 4;      // LOW = something in front of the sensor
const int PIN_DHT = 32;

DHT dht(PIN_DHT, DHT11);

bool cameraAnswers() {
  while (Serial2.available()) Serial2.read();
  Serial2.write('P');
  String reply;
  uint32_t t0 = millis();
  while (millis() - t0 < 300) {
    while (Serial2.available()) reply += (char)Serial2.read();
    if (reply.indexOf("PONG") >= 0) return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17);
  pinMode(PIN_IR, INPUT_PULLUP);
  dht.begin();

  CloudConfig cfg;
  cfg.aws.thingName = THINGNAME;
  cfg.aws.wifiSsid = WIFI_SSID;
  cfg.aws.wifiPassword = WIFI_PASSWORD;
  cfg.aws.endpoint = AWS_IOT_ENDPOINT;
  cfg.aws.rootCa = AWS_CERT_CA;
  cfg.aws.deviceCert = AWS_CERT_CRT;
  cfg.aws.privateKey = AWS_CERT_PRIVATE;
  cfg.heartbeatMs = 2000;  // for the video; the real station uses 15000 (the default)
  cloudBegin(cfg);         // or the one-line cloudBegin(THINGNAME, WIFI_SSID, ...) with the defaults

  cloudSetState(CLOUD_STANDBY);
  cloudSetValve(false);
}

void loop() {
  cloudLoop();  // every loop

  // Temperature and humidity: the DHT11 needs 2 s between reads. NAN on a failed read is fine.
  static uint32_t lastDht = 0;
  if (millis() - lastDht >= 2500) {
    lastDht = millis();
    cloudSetClimate(dht.readTemperature(), dht.readHumidity());
  }

  // The IR level, every loop: 1 s present -> a session (PRESENCE); 0.5 s gone -> it ends (SUMMARY).
  // Level-based, so a bounce or a missed edge can't leave it stuck. No interrupt needed.
  switch (cloudBottleSeen(digitalRead(PIN_IR) == LOW)) {
    case CLOUD_BOTTLE_ARRIVED:
      cloudSetState(CLOUD_DISPENSING);
      Serial.println("bottle: arrived");  // the place to take the photo
      break;
    case CLOUD_BOTTLE_LEFT:
      cloudSetState(CLOUD_STANDBY);
      Serial.println("bottle: left");
      break;
    default:
      break;
  }
  // Also set the state back if the session ended some other way (e.g. the 5-minute watchdog).
  if (!cloudSessionOpen()) cloudSetState(CLOUD_STANDBY);

  // Camera check every 10 s. Two failed pings in a row raise CAM_FAULT, two good ones clear it.
  static uint32_t lastCam = 0;
  if (millis() - lastCam >= 10000) {
    lastCam = millis();
    cloudFaultIf("CAM_FAULT", !cameraAnswers());
    Serial.printf("heap %lu bytes free, %u records waiting\n", (unsigned long)ESP.getFreeHeap(),
                  (unsigned)cloudBuffered());
  }
}

// StationCloud.h - send the station's data to AWS IoT Core by calling functions with plain values.
// The library builds every JSON message itself (message-formats.md v1); the caller never writes JSON.
// This is the only file a sketch includes, and the only one you need to read to use the library.
//
// Adaptive Water Station, group 67. Built on AwsIot.h (Wi-Fi, SNTP clock, MQTT over TLS, QoS 1).
// Plan: SystemArchitecture/communication-plan.md, activity diagram 4.5 (buffer, heartbeat, reconnect).
//
//   #include "secrets.h"
//   #include <StationCloud.h>
//
//   void setup() {
//     cloudBegin(THINGNAME, WIFI_SSID, WIFI_PASSWORD, AWS_IOT_ENDPOINT, AWS_CERT_CA, AWS_CERT_CRT, AWS_CERT_PRIVATE);
//   }
//   void loop() {
//     cloudLoop();                                        // every loop: reconnects, heartbeat, sends the buffer
//     cloudSetClimate(dht.readTemperature(), dht.readHumidity());   // NAN is fine: sent as null + SHT_FAULT
//     if (bottleArrived)  cloudBottlePlaced();             // PRESENCE
//     if (bottleLeft)     cloudFillEnded(CLOUD_BOTTLE_REMOVED);       // SUMMARY
//     if (cameraBroken)   cloudFault("CAM_FAULT");          // FAULT, sent once until cloudFaultCleared
//   }
//
// Where the code is:
//   StationCloud.h                this file: the calls and the settings
//   station_cloud/Types.h         the CLOUD_... values you pass (states, stop reasons, ...)
//   station_cloud/Connection.cpp  cloudBegin, cloudLoop: connecting, heartbeat, publishing the buffer
//   station_cloud/Readings.cpp    cloudSet...: the sensor values
//   station_cloud/Events.cpp      bottles, fills and faults
//   station_cloud/Guards.cpp      cloudBottleSeen, cloudFaultIf, the session watchdog (filters in Debounce.h)
//   station_cloud/Station.h/.cpp  the library's state and how one sample becomes a buffered record
//   station_cloud/Record.h, Queue.h, FaultTable.h, Json.h, Format.h
//                                 plain C++ with no Arduino code, tested on a PC (test/run.mjs)

#pragma once

#include <Arduino.h>
#include <AwsIot.h>

#include "station_cloud/Types.h"

struct CloudConfig {
  AwsIotConfig aws;                            // thing name, Wi-Fi, endpoint, certificates (see AwsIot.h)
  const char *topic = "water/demo/telemetry";  // the 20 Nov demo; later "water/gw-01/telemetry"
  const char *nodeId = "node-01";              // must be registered in the cloud's device table
  uint32_t heartbeatMs = 15000;                // gateway sample. 2000 for the video (plan 1.2 trap 3)
  uint32_t nodeSampleMs = 1000;                // plain node sample, once a node value was set. 0 = only on changes
  uint16_t bufferRecords = 60;                 // about 230 bytes each

  // Guards (cloudBottleSeen, and the session watchdog in cloudLoop)
  uint32_t bottleOnMs = 1000;                  // present this long before it counts as a bottle (message 2)
  uint32_t bottleOffMs = 500;                  // absent this long before it counts as removed
  uint32_t maxSessionMs = 300000;              // a session open longer is closed as TIMEOUT. 0 = never
};

// ---------------------------------------------------------------- start and run (Connection.cpp)

// Starts Wi-Fi, the clock and the MQTT connection. Usually blocks a few seconds; at worst about 35 s
// (Wi-Fi 20 s + clock 15 s), never forever: if something isn't ready, cloudLoop() keeps retrying.
// Returns false only for a missing setting or no memory for the buffer.
bool cloudBegin(const CloudConfig &cfg);

// Same, with the secrets.h names from AWS's tutorial and the default settings above.
bool cloudBegin(const char *thingName, const char *wifiSsid, const char *wifiPassword, const char *endpoint,
                const char *rootCa, const char *deviceCert, const char *privateKey);

// Call in every loop(). Keeps the connection, makes the periodic samples, publishes one buffered record.
// Takes well under a millisecond, except when it publishes (up to 1 s) or reconnects (1-3 s).
void cloudLoop();

bool cloudConnected();      // connected to AWS right now
uint16_t cloudBuffered();   // records waiting to be sent
uint32_t cloudDropped();    // plain records dropped because the buffer was full, not yet reported

// ---------------------------------------------------------------- readings (Readings.cpp)
// NAN = the sensor failed: sent as null, never as 0. Out-of-range values are sent as null too.

// Temperature (°C) and humidity (%RH), e.g. straight from dht.readTemperature() / dht.readHumidity().
// A NAN also lists SHT_FAULT in diag.faults until a good reading. Sent with the next heartbeat.
void cloudSetClimate(float tempC, float humidityPct);

// The dashboard state. A change is sent at once (the 3 s badge in US2-1); the same state again sends nothing.
void cloudSetState(CloudState state);

// Valve open or closed. A change is sent at once.
void cloudSetValve(bool open);

// The rest go out with the next periodic node sample (every nodeSampleMs).
void cloudSetDistance(float mm, int status = 0);
void cloudSetMass(float grams, long raw);
void cloudSetFlowPulses(unsigned long pulsesSinceBoot);
void cloudSetTankLevel(int adcRaw);

// ---------------------------------------------------------------- events (Events.cpp)
// Events are never dropped from the buffer. Each call returns false (or 0) only if it couldn't be stored.

// A bottle has been in the bay long enough: opens a session and sends PRESENCE.
// Returns the session number (1, 2, 3 ...), or 0 if the buffer is full of events (refuse the bottle then).
// If a session was still open, it is first ended as BOTTLE_REMOVED.
uint32_t cloudBottlePlaced(float startMassG = NAN);

// Ends the open session and sends SUMMARY. The duration is measured for you.
//   reason:   why it ended, e.g. CLOUD_BOTTLE_REMOVED, CLOUD_AI_FULL, CLOUD_NOT_A_BOTTLE, CLOUD_NO_FLOW
//   volumeMl: water dispensed (0 if none); source: how it was measured
// False if no session was open.
bool cloudFillEnded(CloudStopReason reason, float volumeMl = 0, CloudVolumeSource source = CLOUD_VOLUME_TIMER,
                    float endMassG = NAN);

// True between cloudBottlePlaced and cloudFillEnded.
bool cloudSessionOpen();

// A fault STARTS: sends FAULT once and lists the code in diag.faults until cloudFaultCleared.
// Calling it again for the same active fault does nothing, so it is safe to call on every check.
//   code: from message-formats.md "Fault codes", e.g. "CAM_FAULT", "NO_FLOW" (A-Z and _, 2-16 characters)
//   alert / sensor: leave as AUTO to take them from that table (station_cloud/FaultTable.h)
// The table also decides which id sends it: CAM_FAULT, SHT_FAULT, LEAK_HUMIDITY on the gateway, the rest on the node.
bool cloudFault(const char *code, CloudAlertType alert = CLOUD_ALERT_AUTO, CloudSensorType sensor = CLOUD_SENSOR_AUTO);

// The fault ENDS: sends FAULT_CLEARED and removes the code from diag.faults. False if it wasn't active.
bool cloudFaultCleared(const char *code);

// ---------------------------------------------------------------- guards (Guards.cpp)
// Use these instead of the calls above when the input is noisy. They look at the LEVEL of the input on
// every call, so a missed interrupt or one odd reading can't leave the state wrong: the next calls fix it.

enum CloudPresence : uint8_t { CLOUD_NO_CHANGE = 0, CLOUD_BOTTLE_ARRIVED, CLOUD_BOTTLE_LEFT };

// Give it the raw sensor reading on every loop, e.g. cloudBottleSeen(digitalRead(PIN_IR) == LOW).
// Present for bottleOnMs -> cloudBottlePlaced(); absent for bottleOffMs -> cloudFillEnded(BOTTLE_REMOVED)
// (if the session is still open). Bounces shorter than that are ignored. Replaces an edge interrupt.
// Returns ARRIVED or LEFT once, when that happens (e.g. take the photo on ARRIVED).
CloudPresence cloudBottleSeen(bool present);

// Give it the result of a health check each time it runs, e.g. cloudFaultIf("CAM_FAULT", !cameraAnswers()).
// The fault starts only after `confirm` bad checks in a row, and clears only after `confirm` good ones,
// so one lost ping doesn't send FAULT + FAULT_CLEARED. Returns whether the fault counts as active.
bool cloudFaultIf(const char *code, bool active, uint8_t confirm = 2);

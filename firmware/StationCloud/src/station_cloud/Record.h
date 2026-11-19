// Record.h - one sample as it waits in the buffer: readings, an optional event, and diag.
// It holds plain values; Format.h turns it into JSON only when it is published. No Arduino dependency.

#pragma once

#include <math.h>
#include <stdint.h>

#include "Types.h"

constexpr int kCloudMaxFaults = 4;
constexpr int kCloudCodeLen = 17;  // a fault code: 16 characters + NUL

// Readings. NAN = "the sensor failed": sent as null. A field the board doesn't have is left out (its
// `has` bit is 0). Out-of-range values are sent as null too (Format.h), because demo/contract.js drops
// the WHOLE message otherwise.
struct CloudReadings {
  enum : uint16_t {
    STATE = 1, VALVE = 2, DISTANCE = 4, DISTANCE_STATUS = 8, MASS_RAW = 16, MASS_G = 32, FLOW = 64,
    TEMP = 128, HUMIDITY = 256, TANK = 512,
  };
  uint16_t has = 0;
  uint8_t state = CLOUD_STATE_UNSET;
  bool valveOpen = false;
  double distanceMm = NAN, distanceStatus = NAN, massRaw = NAN, massG = NAN, flowPulses = NAN;
  double tempC = NAN, humidityPct = NAN, tankLevelRaw = NAN;
};

enum CloudEventType : uint8_t {
  CLOUD_EV_NONE = 0, CLOUD_EV_PRESENCE, CLOUD_EV_SUMMARY, CLOUD_EV_FAULT, CLOUD_EV_FAULT_CLEARED,
};

struct CloudEvent {
  uint8_t type = CLOUD_EV_NONE;
  uint32_t sessionNo = 0;
  double startMassG = NAN, volumeMl = NAN, endMassG = NAN;
  uint8_t stopReason = CLOUD_FAULT;
  uint8_t volumeSource = CLOUD_VOLUME_NONE;
  int32_t durationMs = -1;
  char faultCode[kCloudCodeLen] = "";
  uint8_t alert = CLOUD_ALERT_NONE;
  uint8_t sensor = CLOUD_SENSOR_NONE;
  int32_t faultSession = -1;  // FAULT: the session that was open, or -1
};

struct CloudRecord {
  uint8_t board = CLOUD_NODE;
  uint32_t seq = 0;
  uint32_t monoMs = 0;   // millis() when the sample was taken; measured_at is worked out from it at publish
  uint16_t attempt = 1;  // publish tries so far
  CloudReadings readings;
  CloudEvent event;
  uint8_t nFaults = 0;
  char faults[kCloudMaxFaults][kCloudCodeLen] = {{0}};
  char fwState[kCloudCodeLen] = "";
};

// Types.h - the values a caller passes to StationCloud (states, stop reasons, alert and sensor types).
// No Arduino dependency.

#pragma once

#include <stdint.h>

// The dashboard state (readings.state).
enum CloudState : uint8_t { CLOUD_STATE_UNSET = 0, CLOUD_STANDBY, CLOUD_DISPENSING, CLOUD_ERROR };

// Why a session ended (event.stop_reason, message-formats.md "stop_reason").
enum CloudStopReason : uint8_t {
  CLOUD_AI_FULL = 0,      // option 1: the camera said full                (a filled bottle)
  CLOUD_MASS_TARGET,      // option 2: the load cell reached the target    (a filled bottle)
  CLOUD_BOTTLE_REMOVED,   // taken away before the end
  CLOUD_NOT_A_BOTTLE,     // the AI refused it
  CLOUD_ALREADY_FULL,     // permitted, but already >= 95 % full
  CLOUD_AI_UNAVAILABLE,   // no answer from the AI in time
  CLOUD_UNCERTAIN,        // permitted, but the level can't be read
  CLOUD_NO_FLOW,          // valve open, no flow: the tank is empty        (also call cloudFault("NO_FLOW"))
  CLOUD_TIMEOUT,          // fill took too long                            (also call cloudFault("TIMEOUT"))
  CLOUD_FAULT,            // any other fault stopped it
};

// How volume_ml was measured.
enum CloudVolumeSource : uint8_t { CLOUD_VOLUME_NONE = 0, CLOUD_VOLUME_TIMER, CLOUD_VOLUME_FLOW, CLOUD_VOLUME_MASS };

// AUTO = take it from the fault-code table (FaultTable.h). NONE = send null.
enum CloudAlertType : uint8_t {
  CLOUD_ALERT_AUTO = 0, CLOUD_ALERT_NONE, CLOUD_LOW_WATER, CLOUD_LEAK, CLOUD_DEVICE_OFFLINE, CLOUD_SENSOR_FAULT,
  CLOUD_FILL_ABORTED,
};
enum CloudSensorType : uint8_t {
  CLOUD_SENSOR_AUTO = 0, CLOUD_SENSOR_NONE, CLOUD_TOF, CLOUD_CAMERA, CLOUD_FLOW, CLOUD_LOAD_CELL,
  CLOUD_TEMP_HUMIDITY, CLOUD_TANK_LEVEL,
};

// Which id a record speaks for. One physical board can send both: its own "gateway" sample
// (heartbeat, temperature, humidity) and the "node" sample (bottle, valve, sessions).
enum CloudBoard : uint8_t { CLOUD_NODE = 0, CLOUD_GATEWAY = 1 };

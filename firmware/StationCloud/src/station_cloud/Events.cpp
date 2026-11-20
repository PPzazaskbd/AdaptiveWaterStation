// Events.cpp - bottles, fills and faults (message-formats.md sections 2-5). Each call sends one sample
// that carries the event, at once; event records are never dropped from the buffer.

#include "Station.h"

using namespace cloud_detail;

bool cloudFillEnded(CloudStopReason reason, float volumeMl, CloudVolumeSource source, float endMassG) {
  Station &s = st();
  if (!s.sessionOpen) return false;
  CloudEvent e;
  e.type = CLOUD_EV_SUMMARY;
  e.sessionNo = s.sessionNo;
  e.stopReason = reason;
  e.volumeMl = volumeMl;
  e.volumeSource = source;
  e.durationMs = (int32_t)(millis() - s.sessionStartMs);
  e.startMassG = s.sessionStartMass;
  e.endMassG = endMassG;
  if (reason == CLOUD_NO_FLOW) strcpy(e.faultCode, "NO_FLOW");
  if (reason == CLOUD_TIMEOUT) strcpy(e.faultCode, "TIMEOUT");
  nodeTouched();
  if (!record(CLOUD_NODE, &e)) return false;  // not stored: keep the session open, so a retry can end it
  s.sessionOpen = false;
  return true;
}

uint32_t cloudBottlePlaced(float startMassG) {
  Station &s = st();
  if (s.sessionOpen && !cloudFillEnded(CLOUD_BOTTLE_REMOVED, 0, CLOUD_VOLUME_NONE)) return 0;
  CloudEvent e;
  e.type = CLOUD_EV_PRESENCE;
  e.sessionNo = s.sessionNo + 1;
  e.startMassG = startMassG;
  nodeTouched();
  if (!record(CLOUD_NODE, &e)) return 0;
  s.sessionNo = e.sessionNo;
  s.sessionOpen = true;
  s.sessionStartMs = millis();
  s.sessionStartMass = startMassG;
  return s.sessionNo;
}

bool cloudSessionOpen() { return st().sessionOpen; }

bool cloudFault(const char *code, CloudAlertType alert, CloudSensorType sensor) {
  Station &s = st();
  if (!validFaultCode(code)) {
    if (verbose()) Serial.printf("[cloud] fault code \"%s\" refused: A-Z and _, 2-16 characters\n", code ? code : "");
    return false;
  }
  const FaultInfo *info = findFault(code);
  uint8_t board = info ? info->board : CLOUD_NODE;
  if (!s.faults[board].add(code)) return false;  // already active (or 4 at once)

  CloudEvent e;
  e.type = CLOUD_EV_FAULT;
  strcpy(e.faultCode, code);
  e.alert = alert != CLOUD_ALERT_AUTO ? alert : info ? info->alert : CLOUD_ALERT_NONE;
  e.sensor = sensor != CLOUD_SENSOR_AUTO ? sensor : info ? info->sensor : CLOUD_SENSOR_NONE;
  e.faultSession = board == CLOUD_NODE && s.sessionOpen ? (int32_t)s.sessionNo : -1;
  if (board == CLOUD_NODE) nodeTouched();
  if (record(board, &e)) return true;
  s.faults[board].remove(code);  // not buffered: forget it, so the next call tries again
  return false;
}

bool cloudFaultCleared(const char *code) {
  Station &s = st();
  if (!validFaultCode(code)) return false;
  for (uint8_t board = 0; board < 2; board++) {
    if (!s.faults[board].remove(code)) continue;  // removed first, so this sample no longer lists it
    CloudEvent e;
    e.type = CLOUD_EV_FAULT_CLEARED;
    strcpy(e.faultCode, code);
    if (record(board, &e)) return true;
    // Not stored: keep the fault active, or the cloud's alert would stay open forever with no retry.
    s.faults[board].add(code);
    return false;
  }
  return false;
}

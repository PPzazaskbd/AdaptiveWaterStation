// FaultTable.h - the fault codes (message-formats.md "Fault codes"): which alert and sensor each one
// opens and which id reports it, the code check, and a small list of active faults. No Arduino dependency.

#pragma once

#include <stddef.h>
#include <string.h>

#include "Record.h"

namespace cloud_detail {

struct FaultInfo {
  const char *code;
  CloudAlertType alert;
  CloudSensorType sensor;
  CloudBoard board;
};

// inline: one table shared by every .cpp (the inline functions below must all see the same one).
inline constexpr FaultInfo kFaults[] = {
    {"NO_FLOW", CLOUD_LOW_WATER, CLOUD_SENSOR_NONE, CLOUD_NODE},
    {"TIMEOUT", CLOUD_FILL_ABORTED, CLOUD_SENSOR_NONE, CLOUD_NODE},
    {"LEVEL_LOST", CLOUD_FILL_ABORTED, CLOUD_SENSOR_NONE, CLOUD_NODE},
    {"LINK_LOST", CLOUD_ALERT_NONE, CLOUD_SENSOR_NONE, CLOUD_NODE},
    {"LEAK_FLOW", CLOUD_LEAK, CLOUD_SENSOR_NONE, CLOUD_NODE},
    {"LEAK_HUMIDITY", CLOUD_LEAK, CLOUD_SENSOR_NONE, CLOUD_GATEWAY},
    {"TOF_FAULT", CLOUD_SENSOR_FAULT, CLOUD_TOF, CLOUD_NODE},
    {"MASS_FAULT", CLOUD_SENSOR_FAULT, CLOUD_LOAD_CELL, CLOUD_NODE},
    {"MASS_RANGE", CLOUD_ALERT_NONE, CLOUD_SENSOR_NONE, CLOUD_NODE},
    {"SHT_FAULT", CLOUD_SENSOR_FAULT, CLOUD_TEMP_HUMIDITY, CLOUD_GATEWAY},
    {"CAM_FAULT", CLOUD_SENSOR_FAULT, CLOUD_CAMERA, CLOUD_GATEWAY},
    {"NODE_SILENT", CLOUD_ALERT_NONE, CLOUD_SENSOR_NONE, CLOUD_GATEWAY},
};

// nullptr for a code that isn't in the table (it is still allowed: no alert, sent by the node).
inline const FaultInfo *findFault(const char *code) {
  for (const FaultInfo &f : kFaults)
    if (strcmp(f.code, code) == 0) return &f;
  return nullptr;
}

// fault_code: 2-16 characters, A-Z and _ (the database column).
inline bool validFaultCode(const char *code) {
  if (!code) return false;
  size_t n = strlen(code);
  if (n < 2 || n > 16) return false;
  for (size_t i = 0; i < n; i++)
    if (!((code[i] >= 'A' && code[i] <= 'Z') || code[i] == '_')) return false;
  return true;
}

// The faults active on one id, in the order they started (sent in diag.faults).
struct FaultList {
  uint8_t n = 0;
  char code[kCloudMaxFaults][kCloudCodeLen] = {{0}};

  int find(const char *c) const {
    for (int i = 0; i < n; i++)
      if (strcmp(code[i], c) == 0) return i;
    return -1;
  }
  // false if already listed or the list is full.
  bool add(const char *c) {
    if (find(c) >= 0 || n >= kCloudMaxFaults) return false;
    strncpy(code[n], c, kCloudCodeLen - 1);
    code[n][kCloudCodeLen - 1] = '\0';
    n++;
    return true;
  }
  // false if it wasn't listed.
  bool remove(const char *c) {
    int i = find(c);
    if (i < 0) return false;
    for (int j = i; j + 1 < n; j++) strcpy(code[j], code[j + 1]);
    n--;
    return true;
  }
};

}  // namespace cloud_detail

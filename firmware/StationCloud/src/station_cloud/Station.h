// Station.h - the library's internal state, shared by Connection.cpp, Readings.cpp and Events.cpp.
// Not for sketches: include StationCloud.h instead.

#pragma once

#include "../StationCloud.h"
#include "Debounce.h"
#include "FaultTable.h"
#include "Queue.h"
#include "Record.h"

namespace cloud_detail {

struct Station {
  CloudConfig cfg;
  bool began = false, awsStarted = false, wasUp = false;
  uint32_t lastBeginTry = 0, lastHeartbeat = 0, lastNodeSample = 0;
  bool retryWait = false;     // a publish failed: wait 1 s from retryFrom before the next one
  uint32_t retryFrom = 0;
  char bootId[9] = "";
  uint32_t seq[2] = {0, 0};   // [CLOUD_NODE], [CLOUD_GATEWAY]: each id counts from 0 every boot
  CloudReadings readings[2];  // the latest values, copied into every sample
  FaultList faults[2];        // faults started with cloudFault
  FaultList autoFaults;       // raised by the library itself (SHT_FAULT from cloudSetClimate)
  bool nodeActive = false;    // the node sends plain samples only once something was set on it
  bool sessionOpen = false;
  uint32_t sessionNo = 0, sessionStartMs = 0;
  double sessionStartMass = NAN;
  CloudRecord *slots = nullptr;
  CloudQueue queue;
  char json[1024];

  // Guards.cpp
  CloudDebounce presence;             // cloudBottleSeen
  bool presenceRetry = false;         // the last change couldn't be buffered: retry it
  uint32_t presenceRetryFrom = 0;
  struct Check {                      // cloudFaultIf: one filter per fault code
    char code[kCloudCodeLen] = "";
    CloudConfirm filter;
  };
  Check checks[kCloudMaxFaults];
};

Station &st();
inline bool verbose() { return st().cfg.aws.verbose; }
inline void nodeTouched() { st().nodeActive = true; }

// "node-01" or the thing name.
const char *idOf(uint8_t board);

// Takes a sample of one id now, with an optional event, and buffers it. atFront: the reconnect heartbeat.
// False if the buffer is full of events.
bool record(uint8_t board, const CloudEvent *ev, bool atFront = false);

// Closes a session open longer than cfg.maxSessionMs (Guards.cpp). Called from cloudLoop.
void checkSession();

}  // namespace cloud_detail

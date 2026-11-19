// Station.cpp - the library's state, and how one sample becomes a buffered record.

#include "Station.h"

#include "Format.h"

namespace cloud_detail {

Station &st() {
  static Station s;
  return s;
}

const char *idOf(uint8_t board) { return board == CLOUD_GATEWAY ? st().cfg.aws.thingName : st().cfg.nodeId; }

// diag.fw_state of the gateway: its connection state (firmware-state-machine.md section 4).
static const char *gatewayFwState() {
  if (WiFi.status() != WL_CONNECTED) return "WIFI_CONNECTING";
  if (!awsIotConnected()) return "CLOUD_CONNECTING";
  return st().queue.count() > 0 ? "DRAINING" : "LIVE";
}

bool record(uint8_t board, const CloudEvent *ev, bool atFront) {
  Station &s = st();
  if (!s.slots) return false;
  CloudRecord r;
  r.board = board;
  r.seq = s.seq[board]++;  // +1 for every sample created, sent or not (message-formats.md section 1)
  r.monoMs = millis();
  r.readings = s.readings[board];
  if (ev) r.event = *ev;

  const FaultList &f = s.faults[board];
  for (int i = 0; i < f.n && r.nFaults < kCloudMaxFaults; i++) strcpy(r.faults[r.nFaults++], f.code[i]);
  if (board == CLOUD_GATEWAY) {
    for (int i = 0; i < s.autoFaults.n && r.nFaults < kCloudMaxFaults; i++)
      if (f.find(s.autoFaults.code[i]) < 0) strcpy(r.faults[r.nFaults++], s.autoFaults.code[i]);
    strcpy(r.fwState, gatewayFwState());
  } else {
    uint8_t state = s.readings[CLOUD_NODE].state;
    if (state != CLOUD_STATE_UNSET && state <= CLOUD_ERROR) strcpy(r.fwState, kNodeFw[state]);
  }

  bool ok = atFront ? s.queue.pushFront(r) : s.queue.pushBack(r);
  if (!ok && verbose())
    Serial.printf("[cloud] buffer full of events: %s seq %lu NOT stored\n", idOf(board), (unsigned long)r.seq);
  if (board == CLOUD_NODE) s.lastNodeSample = millis();
  else s.lastHeartbeat = millis();
  return ok;
}

}  // namespace cloud_detail

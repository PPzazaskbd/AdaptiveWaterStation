// Guards.cpp - helpers that keep the station's state machine from going wrong on noisy inputs:
//   cloudBottleSeen  the IR (or any presence) reading -> debounced sessions
//   cloudFaultIf     a health check -> a fault that doesn't flap on one bad reading
//   checkSession     a session that never ends is closed (called from cloudLoop)

#include "Station.h"

using namespace cloud_detail;

CloudPresence cloudBottleSeen(bool present) {
  Station &s = st();
  uint32_t now = millis();

  bool changed = s.presence.update(present, now, s.cfg.bottleOnMs, s.cfg.bottleOffMs);
  // A placement or removal that couldn't be buffered (buffer full of events) is retried once a second.
  bool retryDue = s.presenceRetry && now - s.presenceRetryFrom >= 1000;
  if (!changed && !retryDue) return CLOUD_NO_CHANGE;
  s.presenceRetry = false;

  bool ok;
  if (s.presence.stable()) {
    // Arrived. If a session is somehow still open (a removal was missed), cloudBottlePlaced ends it first.
    ok = cloudBottlePlaced() != 0;
  } else {
    // Left. Nothing to do if the session already ended (filled, refused, timed out).
    ok = !s.sessionOpen || cloudFillEnded(CLOUD_BOTTLE_REMOVED);
  }
  if (!ok) {
    s.presenceRetry = true;
    s.presenceRetryFrom = now;
    return CLOUD_NO_CHANGE;
  }
  return s.presence.stable() ? CLOUD_BOTTLE_ARRIVED : CLOUD_BOTTLE_LEFT;
}

bool cloudFaultIf(const char *code, bool active, uint8_t confirm) {
  Station &s = st();
  if (!validFaultCode(code)) return false;

  // Find this code's filter, or take a free one.
  Station::Check *c = nullptr;
  for (Station::Check &k : s.checks)
    if (k.code[0] && strcmp(k.code, code) == 0) c = &k;
  if (!c) {
    for (Station::Check &k : s.checks)
      if (!k.code[0]) {
        c = &k;
        strcpy(k.code, code);
        k.filter = CloudConfirm();
        break;
      }
  }
  if (!c) {  // more than kCloudMaxFaults different codes: no filter, act directly
    if (active) cloudFault(code);
    else cloudFaultCleared(code);
    return active;
  }

  if (c->filter.update(active, confirm)) {
    bool done = c->filter.stable() ? cloudFault(code) : cloudFaultCleared(code);
    // cloudFault returns false also when the fault is already active: that's fine, nothing to retry.
    bool already = c->filter.stable() == (s.faults[0].find(code) >= 0 || s.faults[1].find(code) >= 0);
    if (!done && !already) c->filter.revert();  // not buffered: try again on the next check
  }
  return c->filter.stable();
}

namespace cloud_detail {

void checkSession() {
  Station &s = st();
  if (!s.sessionOpen || !s.cfg.maxSessionMs || millis() - s.sessionStartMs < s.cfg.maxSessionMs) return;
  if (verbose())
    Serial.printf("[cloud] session %lu open for over %lu s: closing it as TIMEOUT\n", (unsigned long)s.sessionNo,
                  (unsigned long)(s.cfg.maxSessionMs / 1000));
  cloudFillEnded(CLOUD_TIMEOUT);
}

}  // namespace cloud_detail

// Format.h - turns a CloudRecord into the message the cloud expects (message-formats.md section 1, v 1),
// and epoch milliseconds into measured_at. No Arduino dependency: test/ checks it against demo/contract.js.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "Json.h"
#include "Record.h"

namespace cloud_detail {

// Names on the wire, indexed by the enums in Types.h. nullptr = send null / leave out.
// inline: one copy shared by every .cpp, as the inline cloudFormat below requires.
inline constexpr const char *kStates[] = {nullptr, "STANDBY", "DISPENSING", "ERROR"};
inline constexpr const char *kNodeFw[] = {nullptr, "IDLE", "FILLING", "FAULT"};  // fw_state when only the state is known
inline constexpr const char *kStop[] = {"AI_FULL",        "MASS_TARGET", "BOTTLE_REMOVED", "NOT_A_BOTTLE",
                                        "ALREADY_FULL",   "AI_UNAVAILABLE", "UNCERTAIN",   "NO_FLOW",
                                        "TIMEOUT",        "FAULT"};
inline constexpr const char *kSources[] = {nullptr, "TIMER", "FLOW", "MASS"};
inline constexpr const char *kAlerts[] = {nullptr, nullptr, "LOW_WATER", "LEAK", "DEVICE_OFFLINE", "SENSOR_FAULT",
                                          "FILL_ABORTED"};
inline constexpr const char *kSensors[] = {nullptr, nullptr, "TOF", "CAMERA", "FLOW", "LOAD_CELL", "TEMP_HUMIDITY",
                                           "TANK_LEVEL"};

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm); no gmtime needed.
inline void civil(long long days, int &y, unsigned &m, unsigned &d) {
  days += 719468;
  long long era = (days >= 0 ? days : days - 146096) / 146097;
  unsigned doe = (unsigned)(days - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long yy = (long long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yy + (m <= 2));
}

}  // namespace cloud_detail

// UTC milliseconds since 1970 -> "2026-11-15T18:02:12.071Z" (the measured_at format). buf: 25+ bytes.
inline bool cloudIso(long long epochMs, char *buf, size_t len) {
  if (!buf || len < 25 || epochMs < 0) return false;
  long long secs = epochMs / 1000, days = secs / 86400, sod = secs % 86400;
  int y;
  unsigned m, d;
  cloud_detail::civil(days, y, m, d);
  if (y > 9999) return false;
  // The % keep every field's width known to the compiler (no truncation warnings); they change nothing.
  snprintf(buf, len, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", (unsigned)y % 10000u, m % 100u, d % 100u,
           (unsigned)(sod / 3600) % 100u, (unsigned)(sod / 60 % 60), (unsigned)(sod % 60),
           (unsigned)(epochMs % 1000));
  return true;
}

// What the record itself doesn't know, filled in at publish time.
struct CloudFormatInfo {
  const char *nodeId;       // "node-01" or the gateway's thing name
  const char *bootId;       // 8 hex characters, new each boot
  const char *measuredAt;   // cloudIso() of the time the sample was taken
  uint16_t queueDepth;      // records waiting in the buffer
  uint32_t dropped;         // gateway records only: records dropped since the last report
  const char *resetReason;  // seq 0 only: why the board restarted, or nullptr
};

// Writes one message. Returns its length, or -1 if `cap` is too small.
inline int cloudFormat(const CloudRecord &r, const CloudFormatInfo &info, char *out, size_t cap) {
  using namespace cloud_detail;
  Json j(out, cap);
  const CloudReadings &R = r.readings;
  j.open();
  j.key("v"), j.integer(1);
  j.key("node_id"), j.str(info.nodeId);
  j.key("boot_id"), j.str(info.bootId);
  j.key("seq"), j.integer(r.seq);
  j.key("sample_mono_ms"), j.integer(r.monoMs);
  j.key("attempt"), j.integer(r.attempt);
  j.key("measured_at"), j.str(info.measuredAt);

  // Ranges: demo/contract.js READINGS. Outside them the cloud would drop the whole message.
  j.key("readings"), j.open();
  if ((R.has & CloudReadings::STATE) && R.state != CLOUD_STATE_UNSET && R.state <= CLOUD_ERROR)
    j.key("state"), j.str(kStates[R.state]);
  if (R.has & CloudReadings::VALVE) j.key("valve_open"), j.boolean(R.valveOpen);
  if (R.has & CloudReadings::DISTANCE) j.key("distance_mm"), j.ranged(R.distanceMm, 0, 8191, false);
  if (R.has & CloudReadings::DISTANCE_STATUS) j.key("distance_status"), j.ranged(R.distanceStatus, 0, 255, true);
  if (R.has & CloudReadings::MASS_RAW) j.key("mass_raw"), j.ranged(R.massRaw, -8388608, 8388607, true);
  if (R.has & CloudReadings::MASS_G) j.key("mass_g"), j.ranged(R.massG, -1000, 10000, false);
  if (R.has & CloudReadings::FLOW) j.key("flow_pulse_count"), j.ranged(R.flowPulses, 0, 2147483647, true);
  if (R.has & CloudReadings::TEMP) j.key("temp_c"), j.ranged(R.tempC, -40, 125, false);
  if (R.has & CloudReadings::HUMIDITY) j.key("humidity_pct"), j.ranged(R.humidityPct, 0, 100, false);
  if (R.has & CloudReadings::TANK) j.key("tank_level_raw"), j.ranged(R.tankLevelRaw, 0, 4095, true);
  j.close();

  const CloudEvent &e = r.event;
  if (e.type != CLOUD_EV_NONE) {
    j.key("event"), j.open();
    if (e.type == CLOUD_EV_PRESENCE) {
      j.key("type"), j.str("PRESENCE");
      j.key("session_no"), j.integer(e.sessionNo);
      j.key("start_mass_g"), j.ranged(e.startMassG, -1000, 10000, false);
    } else if (e.type == CLOUD_EV_SUMMARY) {
      j.key("type"), j.str("SUMMARY");
      j.key("session_no"), j.integer(e.sessionNo);
      j.key("stop_reason"), j.str(kStop[e.stopReason <= CLOUD_FAULT ? e.stopReason : (uint8_t)CLOUD_FAULT]);
      j.key("volume_ml"), j.ranged(e.volumeMl, 0, 100000, false);
      j.key("volume_source"), j.str(e.volumeSource <= CLOUD_VOLUME_MASS ? kSources[e.volumeSource] : nullptr);
      j.key("duration_ms");
      if (e.durationMs >= 0 && e.durationMs <= 86400000) j.integer(e.durationMs);
      else j.null();
      j.key("start_mass_g"), j.ranged(e.startMassG, -1000, 10000, false);
      j.key("end_mass_g"), j.ranged(e.endMassG, -1000, 10000, false);
      j.key("bottle_profile_id"), j.null();
      if (e.faultCode[0]) j.key("fault_code"), j.str(e.faultCode);
    } else if (e.type == CLOUD_EV_FAULT) {
      j.key("type"), j.str("FAULT");
      j.key("fault_code"), j.str(e.faultCode);
      j.key("alert_type"), j.str(e.alert <= CLOUD_FILL_ABORTED ? kAlerts[e.alert] : nullptr);
      j.key("sensor_type"), j.str(e.sensor <= CLOUD_TANK_LEVEL ? kSensors[e.sensor] : nullptr);
      j.key("session_no");
      if (e.faultSession >= 0) j.integer(e.faultSession);
      else j.null();
    } else {
      j.key("type"), j.str("FAULT_CLEARED");
      j.key("fault_code"), j.str(e.faultCode);
    }
    j.close();
  }

  j.key("diag"), j.open();
  j.key("faults"), j.open('[');
  for (int i = 0; i < r.nFaults && i < kCloudMaxFaults; i++) j.str(r.faults[i]);
  j.close(']');
  j.key("queue_depth"), j.integer(info.queueDepth);
  if (r.fwState[0]) j.key("fw_state"), j.str(r.fwState);
  if (r.board == CLOUD_GATEWAY) {
    j.key("buffer"), j.integer(info.queueDepth);
    if (info.dropped) j.key("dropped"), j.integer(info.dropped);
  }
  if (info.resetReason && r.seq == 0) j.key("reset_reason"), j.str(info.resetReason);
  j.close();

  j.close();
  return j.ok() ? (int)j.length() : -1;
}

// PC test of the plain-C++ parts (src/station_cloud/*.h): buffer, fault table, JSON, one line per message type.
// Run:  node firmware/StationCloud/test/run.mjs   (compiles this with g++, then checks every line
//       against demo/contract.js, the checker the cloud demo uses)

#include "../src/station_cloud/Debounce.h"
#include "../src/station_cloud/FaultTable.h"
#include "../src/station_cloud/Format.h"
#include "../src/station_cloud/Queue.h"

#include <assert.h>

static int failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);        \
      failures++;                                                 \
    }                                                             \
  } while (0)

static void emit(const CloudRecord &r, const char *id, uint16_t depth = 0, uint32_t dropped = 0,
                 const char *reset = nullptr) {
  char at[32], out[1024];
  cloudIso(1794765732071LL, at, sizeof at);  // 2026-11-15T18:02:12.071Z
  CloudFormatInfo info{id, "1f08e6d9", at, depth, dropped, reset};
  int n = cloudFormat(r, info, out, sizeof out);
  CHECK(n > 0);
  printf("%s\n", out);
}

static CloudRecord plain(uint32_t seq, uint8_t board = CLOUD_NODE) {
  CloudRecord r;
  r.board = board;
  r.seq = seq;
  r.monoMs = seq * 1000;
  return r;
}

static void testIso() {
  char b[32];
  cloudIso(1794765732071LL, b, sizeof b);
  CHECK(strcmp(b, "2026-11-15T18:02:12.071Z") == 0);
  cloudIso(0, b, sizeof b);
  CHECK(strcmp(b, "1970-01-01T00:00:00.000Z") == 0);
  cloudIso(1709210096789LL, b, sizeof b);  // leap day
  CHECK(strcmp(b, "2024-02-29T12:34:56.789Z") == 0);
}

static void testQueue() {
  CloudRecord slots[3];
  CloudQueue q;
  q.init(slots, 3);
  CloudRecord ev = plain(1);
  ev.event.type = CLOUD_EV_PRESENCE;
  CHECK(q.pushBack(plain(0)));
  CHECK(q.pushBack(ev));
  CHECK(q.pushBack(plain(2)));
  CHECK(q.pushBack(plain(3)));  // full: drops seq 0, the oldest plain one
  CHECK(q.count() == 3 && q.dropped() == 1);
  CHECK(q.at(0).seq == 1 && q.at(1).seq == 2 && q.at(2).seq == 3);  // order kept

  CHECK(q.pushFront(plain(9, CLOUD_GATEWAY)));  // heartbeat first: drops seq 2
  CHECK(q.at(0).seq == 9 && q.at(1).seq == 1 && q.at(2).seq == 3 && q.dropped() == 2);
  q.popFront();
  CHECK(q.front()->seq == 1);

  // All events: nothing can be dropped, the new record is refused.
  CloudQueue e;
  e.init(slots, 3);
  for (int i = 0; i < 3; i++) CHECK(e.pushBack(ev));
  CHECK(!e.pushBack(plain(5)));
  CHECK(e.count() == 3);
  e.forgetDropped(5);
  CHECK(e.dropped() == 0);

  // Ring wrap-around.
  CloudQueue w;
  w.init(slots, 3);
  for (uint32_t i = 0; i < 10; i++) {
    CHECK(w.pushBack(plain(i)));
    if (w.count() == 2) w.popFront();
  }
  CHECK(w.count() == 1 && w.front()->seq == 9);
}

static void testFaultCodes() {
  CHECK(cloud_detail::validFaultCode("CAM_FAULT"));
  CHECK(!cloud_detail::validFaultCode("cam_fault"));
  CHECK(!cloud_detail::validFaultCode("A"));
  CHECK(!cloud_detail::validFaultCode("SEVENTEEN_CHARSXX"));
  CHECK(cloud_detail::findFault("CAM_FAULT")->board == CLOUD_GATEWAY);
  CHECK(cloud_detail::findFault("NO_FLOW")->alert == CLOUD_LOW_WATER);

  cloud_detail::FaultList l;
  CHECK(l.add("A_A") && !l.add("A_A") && l.add("B_B") && l.n == 2);  // no duplicates
  CHECK(l.remove("A_A") && !l.remove("A_A") && l.n == 1 && strcmp(l.code[0], "B_B") == 0);
  CHECK(l.add("C_C") && l.add("D_D") && l.add("E_E") && !l.add("F_F"));  // at most 4
}

// The IR sensor: bounces are ignored, and a wrong state is always corrected by the next readings.
static void testDebounce() {
  CloudDebounce d;
  const uint32_t on = 1000, off = 500;
  // Bouncing for 0.9 s never counts as a bottle.
  for (uint32_t t = 0; t < 900; t += 50) CHECK(!d.update((t / 50) % 2 == 0, t, on, off));
  CHECK(!d.stable());
  // Steady for 1 s: accepted once.
  int changes = 0;
  for (uint32_t t = 900; t <= 2000; t += 10) changes += d.update(true, t, on, off);
  CHECK(changes == 1 && d.stable());
  // One stray "absent" reading in a fill doesn't end the session.
  CHECK(!d.update(false, 2010, on, off));
  for (uint32_t t = 2020; t < 3000; t += 10) CHECK(!d.update(true, t, on, off));
  CHECK(d.stable());
  // The edge-interrupt failure: the code missed the removal. Level readings still catch it.
  changes = 0;
  for (uint32_t t = 3000; t <= 3600; t += 100) changes += d.update(false, t, on, off);
  CHECK(changes == 1 && !d.stable());
  // millis() wrap-around doesn't confuse it.
  CloudDebounce w;
  uint32_t t0 = 0xFFFFFF00u;
  CHECK(!w.update(true, t0, on, off));
  CHECK(w.update(true, t0 + 1000, on, off) && w.stable());

  // The camera ping: one failure doesn't raise the fault, two in a row do; the same to clear it.
  CloudConfirm c;
  CHECK(!c.update(true, 2) && !c.stable());
  CHECK(!c.update(false, 2));               // a good ping resets the count
  CHECK(!c.update(true, 2));
  CHECK(c.update(true, 2) && c.stable());   // two bad in a row: fault
  CHECK(!c.update(false, 2) && c.stable());
  CHECK(c.update(false, 2) && !c.stable()); // two good in a row: cleared
  c.update(true, 2), c.update(true, 2);
  c.revert();                               // couldn't be sent: accepted again after 2 more
  CHECK(!c.stable() && !c.update(true, 2) && c.update(true, 2));
}

int main() {
  testIso();
  testDebounce();
  testQueue();
  testFaultCodes();

  // 1. gateway heartbeat, first after boot, with a DHT11 reading
  CloudRecord g = plain(0, CLOUD_GATEWAY);
  g.readings.has = CloudReadings::TEMP | CloudReadings::HUMIDITY;
  g.readings.tempC = 28.1;
  g.readings.humidityPct = 64.7;
  strcpy(g.fwState, "LIVE");
  emit(g, "gw-01", 0, 0, "POWERON");

  // 2. gateway heartbeat, DHT11 failed (NAN) and out of range: nulls, SHT_FAULT, dropped count
  CloudRecord g2 = plain(5, CLOUD_GATEWAY);
  g2.readings.has = CloudReadings::TEMP | CloudReadings::HUMIDITY;
  g2.readings.tempC = NAN;
  g2.readings.humidityPct = 140;  // a broken sensor; the contract would reject 140
  g2.nFaults = 1;
  strcpy(g2.faults[0], "SHT_FAULT");
  strcpy(g2.fwState, "DRAINING");
  emit(g2, "gw-01", 7, 3);

  // 3. plain node sample with every reading
  CloudRecord n = plain(31);
  CloudReadings &R = n.readings;
  R.has = 0x3FF;
  R.state = CLOUD_DISPENSING;
  R.valveOpen = true;
  R.distanceMm = 64;
  R.distanceStatus = 0;
  R.massRaw = 512844;
  R.massG = 1029.6;
  R.flowPulses = 2911;
  R.tempC = 27.0;
  R.humidityPct = 60.25;
  R.tankLevelRaw = 3170;
  strcpy(n.fwState, "FILLING");
  emit(n, "node-01");

  // 4. PRESENCE
  CloudRecord p = plain(2);
  p.readings.has = CloudReadings::STATE;
  p.readings.state = CLOUD_STANDBY;
  p.event.type = CLOUD_EV_PRESENCE;
  p.event.sessionNo = 1;
  emit(p, "node-01");

  // 5. SUMMARY, bottle removed, no mass
  CloudRecord s = p;
  s.seq = 3;
  s.event = CloudEvent();
  s.event.type = CLOUD_EV_SUMMARY;
  s.event.sessionNo = 1;
  s.event.stopReason = CLOUD_BOTTLE_REMOVED;
  s.event.volumeMl = 0;
  s.event.volumeSource = CLOUD_VOLUME_TIMER;
  s.event.durationMs = 4200;
  emit(s, "node-01");

  // 6. SUMMARY, tank dry, with fault_code
  CloudRecord d = s;
  d.seq = 4;
  d.readings.state = CLOUD_ERROR;
  d.event.stopReason = CLOUD_NO_FLOW;
  d.event.volumeMl = 212.4;
  d.event.volumeSource = CLOUD_VOLUME_FLOW;
  strcpy(d.event.faultCode, "NO_FLOW");
  d.nFaults = 1;
  strcpy(d.faults[0], "NO_FLOW");
  emit(d, "node-01");

  // 7. FAULT with a session, and a gateway CAM_FAULT without one
  CloudRecord f = plain(5);
  f.event.type = CLOUD_EV_FAULT;
  strcpy(f.event.faultCode, "NO_FLOW");
  f.event.alert = CLOUD_LOW_WATER;
  f.event.sensor = CLOUD_SENSOR_NONE;
  f.event.faultSession = 1;
  emit(f, "node-01");
  CloudRecord c = plain(6, CLOUD_GATEWAY);
  c.event.type = CLOUD_EV_FAULT;
  strcpy(c.event.faultCode, "CAM_FAULT");
  c.event.alert = CLOUD_SENSOR_FAULT;
  c.event.sensor = CLOUD_CAMERA;
  emit(c, "gw-01");

  // 8. FAULT_CLEARED
  CloudRecord fc = plain(7);
  fc.event.type = CLOUD_EV_FAULT_CLEARED;
  strcpy(fc.event.faultCode, "NO_FLOW");
  emit(fc, "node-01");

  // Too small a buffer is reported, never overrun.
  char tiny[40];
  CloudFormatInfo info{"node-01", "1f08e6d9", "2026-11-15T18:02:12.071Z", 0, 0, nullptr};
  CHECK(cloudFormat(n, info, tiny, sizeof tiny) == -1);

  fprintf(stderr, failures ? "%d C++ checks FAILED\n" : "all C++ checks passed\n", failures);
  return failures ? 1 : 0;
}

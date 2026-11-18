// Readings.cpp - cloudSet...: the latest sensor values, copied into every sample of their id.

#include "Station.h"

using namespace cloud_detail;

namespace {

// Sets a node reading; it goes out with the next periodic node sample.
void setNode(uint16_t bit, double CloudReadings::*field, double v) {
  CloudReadings &r = st().readings[CLOUD_NODE];
  r.has |= bit;
  r.*field = v;
  nodeTouched();
}

}  // namespace

void cloudSetClimate(float tempC, float humidityPct) {
  Station &s = st();
  CloudReadings &r = s.readings[CLOUD_GATEWAY];
  r.has |= CloudReadings::TEMP | CloudReadings::HUMIDITY;
  r.tempC = tempC;
  r.humidityPct = humidityPct;
  // A failed read (NAN) or an impossible value (the cloud's ranges) both mean the sensor is faulty.
  bool bad = !(tempC >= -40 && tempC <= 125) || !(humidityPct >= 0 && humidityPct <= 100);
  if (bad) s.autoFaults.add("SHT_FAULT");
  else s.autoFaults.remove("SHT_FAULT");
}

void cloudSetState(CloudState state) {
  CloudReadings &r = st().readings[CLOUD_NODE];
  bool changed = !(r.has & CloudReadings::STATE) || r.state != state;
  r.has |= CloudReadings::STATE;
  r.state = state;
  nodeTouched();
  if (changed) record(CLOUD_NODE, nullptr);
}

void cloudSetValve(bool open) {
  CloudReadings &r = st().readings[CLOUD_NODE];
  bool changed = !(r.has & CloudReadings::VALVE) || r.valveOpen != open;
  r.has |= CloudReadings::VALVE;
  r.valveOpen = open;
  nodeTouched();
  if (changed) record(CLOUD_NODE, nullptr);
}

void cloudSetDistance(float mm, int status) {
  setNode(CloudReadings::DISTANCE, &CloudReadings::distanceMm, mm);
  setNode(CloudReadings::DISTANCE_STATUS, &CloudReadings::distanceStatus, status);
}

void cloudSetMass(float grams, long raw) {
  setNode(CloudReadings::MASS_G, &CloudReadings::massG, grams);
  setNode(CloudReadings::MASS_RAW, &CloudReadings::massRaw, (double)raw);
}

void cloudSetFlowPulses(unsigned long pulsesSinceBoot) {
  setNode(CloudReadings::FLOW, &CloudReadings::flowPulses, (double)pulsesSinceBoot);
}

void cloudSetTankLevel(int adcRaw) { setNode(CloudReadings::TANK, &CloudReadings::tankLevelRaw, adcRaw); }

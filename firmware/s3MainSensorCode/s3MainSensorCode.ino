#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <HX711.h>
#include "secrets.h"
#include <StationCloud.h>

// --- PIN DEFINITIONS (ESP32-WROOM) ---
#define DHTPIN 32
#define DHTTYPE DHT11
const int SENSOR_PIN_OBSTACLE = 4;  // T447 IR obstacle sensor
const int SENSOR_PIN_FLOW     = 19; // YF-S401 Flow sensor yellow wire
const int LOADCELL_DOUT_PIN   = 25; // HX711 DT pin
const int LOADCELL_SCK_PIN    = 26; // HX711 SCK pin

DHT dht(DHTPIN, DHTTYPE);
HX711 scale;
WebServer server(80);

// --- CLIMATE STATE ---
float airHumidity = NAN;
float airTemp = NAN;

// --- OBSTACLE & SESSION STATE ---
bool obstaclePresent = false;
unsigned long bottleCount = 0;

// --- YF-S401 FLOW SENSOR STATE ---
volatile unsigned long totalFlowPulses = 0; // Cumulative since boot for cloudSetFlowPulses()
unsigned long lastSecondPulses = 0;
unsigned long sessionStartPulses = 0;
float flowRateLmin = 0.0;
float totalWaterML = 0.0;
unsigned long lastFlowRead = 0;
const float YFS401_FACTOR = 98.0; // 98 Hz = 1 L/min (5880 pulses/L)

// --- 1KG LOAD CELL (HX711) STATE ---
float currentMassGrams = 0.0;
long currentMassRaw = 0;
float CAL_FACTOR_GRAMS = 420.0; // Adjust during calibration with a known weight

// --- CAMERA STATE ---
const size_t MAX_JPG = 40000;
uint8_t* latestBottleJpg = nullptr;
size_t latestBottleLen = 0;
bool cameraHealthy = false;

// [YF-S401] Hardware Interrupt: counts every pulse since boot
void IRAM_ATTR handleFlowInterrupt() {
  totalFlowPulses++;
}

// Sends 'P' (Ping) to verify ESP32-CAM is responsive
bool checkCameraStatus() {
  while (Serial2.available()) Serial2.read();
  Serial.println("[DIAGNOSTIC] Sending Ping ('P') to ESP32-CAM...");
  Serial2.write('P');

  unsigned long start = millis();
  String response = "";
  while (millis() - start < 1500) {
    if (Serial2.available()) {
      char c = Serial2.read();
      response += c;
      if (response.indexOf("PONG") != -1) return true;
    }
  }
  return false;
}

// Requests a JPEG frame from ESP-32S CAM over Serial2 (RX2=16, TX2=17)
bool requestPhotoFromCam(uint8_t* destBuf, size_t* destLen) {
  if (!destBuf) return false;
  while (Serial2.available()) Serial2.read();

  Serial.println("[CAM] Sending 'C' command to ESP32-CAM...");
  Serial2.write('C');

  unsigned long start = millis();
  int match = 0;
  const char* hdr = "JPG!";
  while (millis() - start < 3000) {
    if (Serial2.available()) {
      char c = Serial2.read();
      if (c == hdr[match]) {
        match++;
        if (match == 4) break;
      } else {
        match = (c == hdr[0]) ? 1 : 0;
      }
    }
  }

  if (match != 4) {
    Serial.println("[CAM ERROR] Timeout waiting for 'JPG!' header.");
    return false;
  }

  uint32_t imgSize = 0;
  if (Serial2.readBytes((uint8_t*)&imgSize, 4) != 4) return false;
  if (imgSize == 0 || imgSize > MAX_JPG) return false;

  size_t got = Serial2.readBytes(destBuf, imgSize);
  if (got != imgSize) return false;

  *destLen = imgSize;
  Serial.printf("[CAM SUCCESS] Captured snapshot (%u bytes)!\n", imgSize);
  return true;
}

void sendJpeg(const uint8_t* buf, size_t len) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "image/jpeg", (const char*)buf, len);
}

void handleRoot() {
  String statusText = obstaclePresent ? "BOTTLE PRESENT (Filling)" : "CLEAR (Standby)";
  String camStatusText = cameraHealthy ? "<span style='color:#28a745;'>ONLINE</span>" : "<span style='color:#dc3545;'>OFFLINE</span>";
  String awsStatusText = cloudConnected() ? "<span style='color:#28a745;'>CONNECTED</span>" : "<span style='color:#ffc107;'>BUFFERING (" + String(cloudBuffered()) + ")</span>";

  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>body{font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:20px;}"
                "img{width:320px;max-width:100%;border-radius:8px;border:2px solid #444;margin:10px 0;}"
                ".status-card{background:#222;padding:12px 16px;border-radius:8px;margin:8px;border:1px solid #333;min-width:130px;}"
                "button{padding:10px 18px;font-size:16px;border-radius:6px;cursor:pointer;background:#007bff;color:white;border:none;margin:5px;}</style></head><body>"
                "<h2>Smart Water Dispenser (WROOM + AWS IoT)</h2>"
                "<p>AWS Cloud: <b>" + awsStatusText + "</b> | Camera: <b>" + camStatusText + "</b></p>"
                "<div style='display:flex; justify-content:center; flex-wrap:wrap;'>"
                  "<div class='status-card'>Humidity<br><b style='font-size:22px; color:#00d2fc;'>" + (isnan(airHumidity) ? "--" : String(airHumidity, 1) + "%") + "</b></div>"
                  "<div class='status-card'>Temperature<br><b style='font-size:22px; color:#ff7b00;'>" + (isnan(airTemp) ? "--" : String(airTemp, 1) + "&deg;C") + "</b></div>"
                  "<div class='status-card'>Flow Rate<br><b style='font-size:22px; color:#00ff88;'>" + String(flowRateLmin, 2) + " L/min</b></div>"
                  "<div class='status-card'>Dispensed<br><b style='font-size:22px; color:#ffc107;'>" + String(totalWaterML, 1) + " mL</b></div>"
                  "<div class='status-card'>Bottle Mass<br><b style='font-size:22px; color:#e83e8c;'>" + String(currentMassGrams, 1) + " g</b></div>"
                "</div>"
                "<p>Bay Status: <b>" + statusText + "</b> | Total Bottles: <b>" + String(bottleCount) + "</b></p>"
                "<h3>Latest Snapshot</h3>";

  if (latestBottleLen == 0) {
    html += "<p style='color:#888;'>No photo captured yet.</p>";
  } else {
    html += "<img src='/latest.jpg?t=" + String(millis()) + "'><br><br>";
  }

  html += "<button onclick=\"location.reload()\">Refresh</button>"
          "<button style='background:#444;' onclick=\"location.href='/reset_flow'\">Reset mL</button>"
          "<button style='background:#28a745;' onclick=\"location.href='/tare'\">Tare Scale (0g)</button>"
          "</body></html>";
  server.send(200, "text/html", html);
}

void handleLatestJpg() {
  if (latestBottleLen == 0) {
    server.send(404, "text/plain", "No snapshot captured yet!");
    return;
  }
  sendJpeg(latestBottleJpg, latestBottleLen);
}

void handleResetFlow() {
  totalWaterML = 0.0;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleTare() {
  if (scale.is_ready()) {
    scale.tare();
    Serial.println("[SCALE] Load cell tared to 0.0 g!");
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial2.begin(115200, SERIAL_8N1, 16, 17); // RX2=16, TX2=17
  Serial2.setTimeout(2500);

  latestBottleJpg = (uint8_t*)malloc(MAX_JPG);

  // Initialize sensors
  dht.begin(15);
  pinMode(SENSOR_PIN_OBSTACLE, INPUT_PULLUP);
  pinMode(SENSOR_PIN_FLOW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN_FLOW), handleFlowInterrupt, FALLING);
  lastFlowRead = millis();

  // Initialize 1kg Load Cell (HX711 on GPIO 25 / 26)
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(CAL_FACTOR_GRAMS);
  if (scale.wait_ready_timeout(500)) {
    scale.tare();
    Serial.println("[SCALE] HX711 Load Cell Ready & Tared to 0g!");
  } else {
    Serial.println("[SCALE WARN] HX711 not detected yet (check GPIO 25/26 wiring).");
  }

  // --- START WIFI, CLOCK & AWS IOT CORE (via StationCloud) ---
  Serial.println("[CLOUD] Starting Wi-Fi, SNTP, and AWS IoT Core...");
  cloudBegin(THINGNAME, WIFI_SSID, WIFI_PASSWORD, AWS_IOT_ENDPOINT,
             AWS_CERT_CA, AWS_CERT_CRT, AWS_CERT_PRIVATE);

  Serial.print("OPEN DASHBOARD IN BROWSER: http://");
  Serial.println(WiFi.localIP());

  // --- CAMERA DIAGNOSTIC CHECK ---
  cameraHealthy = checkCameraStatus();
  cloudFaultIf("CAM_FAULT", !cameraHealthy);

  server.on("/", handleRoot);
  server.on("/latest.jpg", handleLatestJpg);
  server.on("/reset_flow", handleResetFlow);
  server.on("/tare", handleTare);
  server.begin();
}

void loop() {
  server.handleClient();
  cloudLoop(); // Reconnects, sends heartbeat, flushes QoS 1 buffer

  // 1. Bottle Presence Guard (replaces noisy edge interrupts for AWS)
  CloudPresence edge = cloudBottleSeen(digitalRead(SENSOR_PIN_OBSTACLE) == LOW);
  if (edge == CLOUD_BOTTLE_ARRIVED) {
    obstaclePresent = true;
    bottleCount++;
    sessionStartPulses = totalFlowPulses;
    Serial.printf("\n[SENSOR] Bottle #%lu Confirmed (1s)! Requesting snapshot...\n", bottleCount);

    if (cameraHealthy) {
      bool ok = requestPhotoFromCam(latestBottleJpg, &latestBottleLen);
      cloudFaultIf("CAM_FAULT", !ok);
    }
  } else if (edge == CLOUD_BOTTLE_LEFT) {
    obstaclePresent = false;
    Serial.println("[SENSOR] Bottle Removed (SUMMARY queued to AWS).");
  }

  // 2. Flow Sensor + Load Cell update every 1000ms
  unsigned long now = millis();
  if (now - lastFlowRead >= 1000) {
    unsigned long elapsed = now - lastFlowRead;
    lastFlowRead = now;

    noInterrupts();
    unsigned long currentTotalPulses = totalFlowPulses;
    interrupts();

    unsigned long deltaPulses = currentTotalPulses - lastSecondPulses;
    lastSecondPulses = currentTotalPulses;

    float hz = (deltaPulses * 1000.0f) / (float)elapsed;
    flowRateLmin = hz / YFS401_FACTOR;
    totalWaterML += (deltaPulses * 1000.0f) / (YFS401_FACTOR * 60.0f);

    // Send cumulative pulses to StationCloud
    cloudSetFlowPulses(currentTotalPulses);

    // Non-blocking HX711 Load Cell read
    if (scale.is_ready()) {
      currentMassRaw = scale.read();
      currentMassGrams = scale.get_units(1);
      cloudSetMass(currentMassGrams, currentMassRaw);
    }

    if (deltaPulses > 0) {
      Serial.printf("[FLOW] Rate: %.2f L/min | Total: %.1f mL | Mass: %.1f g\n",
                    flowRateLmin, totalWaterML, currentMassGrams);
    }
  }

  // 3. Climate (DHT11) update every 2.5 seconds
  static unsigned long lastDHTRead = 0;
  if (now - lastDHTRead > 2500) {
    lastDHTRead = now;
    airHumidity = dht.readHumidity();
    airTemp = dht.readTemperature();
    cloudSetClimate(airTemp, airHumidity); // NAN is automatically handled as null + SHT_FAULT
  }

  // 4. Serial Monitor manual commands ('c' = photo, 't' = tare scale, 'r' = reset flow)
  if (Serial.available()) {
    char key = Serial.read();
    if (key == 'c' || key == 'C') {
      cameraHealthy = checkCameraStatus();
      cloudFaultIf("CAM_FAULT", !cameraHealthy);
      if (cameraHealthy) requestPhotoFromCam(latestBottleJpg, &latestBottleLen);
    } else if (key == 't' || key == 'T') {
      if (scale.is_ready()) {
        scale.tare();
        Serial.println("[SCALE] Tared to 0.0 g!");
      }
    } else if (key == 'r' || key == 'R') {
      totalWaterML = 0.0;
      Serial.println("[FLOW] Reset total water mL to 0.");
    }
  }
}

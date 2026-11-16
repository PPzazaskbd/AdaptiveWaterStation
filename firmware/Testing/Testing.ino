#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

// 1. ENTER YOUR 2.4GHz WIFI CREDENTIALS
const char* WIFI_SSID = "YOUR SSID";
const char* WIFI_PASS = "YOUR PWD";

const int SENSOR_PIN = 4; // T447 IR obstacle sensor
HardwareSerial CamSerial(1); // UART1 to talk to ESP-32S CAM
WebServer server(80);

// Interrupt state for T447 IR sensor
volatile bool sensorChanged = false;
volatile unsigned long lastTriggerTime = 0;
const unsigned long DEBOUNCE_DELAY = 150;
bool obstaclePresent = false;
unsigned long bottleCount = 0;

// Buffers in S3's 8MB PSRAM
const size_t MAX_JPG = 100000;
uint8_t* latestBottleJpg = nullptr;
size_t latestBottleLen = 0;
uint8_t* liveJpg = nullptr;
size_t liveLen = 0;

void IRAM_ATTR handleSensorInterrupt() {
  unsigned long now = millis();
  if (now - lastTriggerTime > DEBOUNCE_DELAY) {
    sensorChanged = true;
    lastTriggerTime = now;
  }
}

// Requests a JPEG frame from ESP-32S CAM over UART1 (GPIO 17/18)
bool requestPhotoFromCam(uint8_t* destBuf, size_t* destLen) {
  if (!destBuf) return false;
  while (CamSerial.available()) CamSerial.read(); // Flush old bytes

  CamSerial.write('C'); // Request capture

  // Wait up to 2.5s for "JPG!" header
  unsigned long start = millis();
  int match = 0;
  const char* hdr = "JPG!";
  while (millis() - start < 2500) {
    if (CamSerial.available()) {
      char c = CamSerial.read();
      if (c == hdr[match]) {
        match++;
        if (match == 4) break;
      } else {
        match = (c == hdr[0]) ? 1 : 0;
      }
    }
  }
  if (match != 4) return false;

  // Read 4-byte image size
  uint32_t imgSize = 0;
  if (CamSerial.readBytes((uint8_t*)&imgSize, 4) != 4) return false;
  if (imgSize == 0 || imgSize > MAX_JPG) return false;

  // Read exact JPEG bytes
  size_t got = CamSerial.readBytes(destBuf, imgSize);
  if (got != imgSize) return false;

  *destLen = imgSize;
  return true;
}

void sendJpeg(const uint8_t* buf, size_t len) {
  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: image/jpeg");
  client.println("Cache-Control: no-store");
  client.printf("Content-Length: %u\r\n\r\n", len);
  client.write(buf, len);
}

void handleRoot() {
  String statusText = obstaclePresent ? "BOTTLE PRESENT (Filling)" : "CLEAR (Standby)";
  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>body{font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:20px;}"
                "img{width:320px;max-width:100%;border-radius:8px;border:2px solid #444;margin:10px 0;}"
                "button{padding:10px 18px;font-size:16px;border-radius:6px;cursor:pointer;}</style></head><body>"
                "<h2>Smart Water Dispenser Dashboard (ESP32-S3)</h2>"
                "<p>Sensor Status: <b>" + statusText + "</b> | Total Bottles: <b>" + String(bottleCount) + "</b></p>"
                "<h3>Latest Bottle-Triggered Snapshot</h3>"
                "<img src='/latest.jpg?t=" + String(millis()) + "' alt='Place a bottle to capture...'><br>"
                "<h3>Live Camera View</h3>"
                "<img src='/live.jpg?t=" + String(millis()) + "'><br>"
                "<button onclick=\"location.reload()\">Capture Live & Refresh</button>"
                "</body></html>";
  server.send(200, "text/html", html);
}

void handleLiveJpg() {
  if (!requestPhotoFromCam(liveJpg, &liveLen)) {
    server.send(500, "text/plain", "CAM UART timeout - check GPIO 17/18 wiring");
    return;
  }
  sendJpeg(liveJpg, liveLen);
}

void handleLatestJpg() {
  if (latestBottleLen == 0) {
    server.send(404, "text/plain", "No bottle triggered yet!");
    return;
  }
  sendJpeg(latestBottleJpg, latestBottleLen);
}

void setup() {
  Serial.begin(115200);
  CamSerial.begin(230400, SERIAL_8N1, 18, 17); // RX=18, TX=17
  CamSerial.setTimeout(2500);

  latestBottleJpg = psramFound() ? (uint8_t*)ps_malloc(MAX_JPG) : (uint8_t*)malloc(40000);
  liveJpg         = psramFound() ? (uint8_t*)ps_malloc(MAX_JPG) : (uint8_t*)malloc(40000);

  pinMode(SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), handleSensorInterrupt, CHANGE);
  obstaclePresent = (digitalRead(SENSOR_PIN) == LOW);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("ESP32-S3 connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected!");
  Serial.print("OPEN DASHBOARD IN BROWSER: http://");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/live.jpg", handleLiveJpg);
  server.on("/latest.jpg", handleLatestJpg);
  server.begin();
}

void loop() {
  server.handleClient();

  if (sensorChanged) {
    sensorChanged = false;
    bool current = (digitalRead(SENSOR_PIN) == LOW);
    if (current != obstaclePresent) {
      obstaclePresent = current;
      if (obstaclePresent) {
        bottleCount++;
        Serial.printf("Bottle #%lu Detected! Requesting snapshot from CAM...\n", bottleCount);
        if (requestPhotoFromCam(latestBottleJpg, &latestBottleLen)) {
          Serial.printf("Snapshot received! (%u bytes)\n", latestBottleLen);
        } else {
          Serial.println("CAM snapshot timeout - check GPIO 17/18 to U0R/U0T");
        }
      } else {
        Serial.println("Bottle Removed.");
      }
    }
  }
}

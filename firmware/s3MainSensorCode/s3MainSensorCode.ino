#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#define DHTPIN 32        // DHT11 Data pin connected to GPIO 5
#define DHTTYPE DHT11     // Defining the sensor type as DHT11
DHT dht(DHTPIN, DHTTYPE); // Initialize the DHT object

float airHumidity = 0.0;  // Stores the relative humidity percentage
float airTemp = 0.0;      // Optional: Stores temperature in Celsius

const char* WIFI_SSID = "YOUR WIFI SSID";
const char* WIFI_PASS = "YOUR PASSWORD";

const int SENSOR_PIN_OBSTACLE = 4; //T447 IR obstacle sensor on GPIO 4
WebServer server(80);

volatile bool sensorChanged = false; //sensor change check
volatile unsigned long lastTriggerTime = 0; //timestamp of the last sensor trigger
const unsigned long DEBOUNCE_DELAY = 150; //debouncing
bool obstaclePresent = false;  //check for object (obstacle)
unsigned long bottleCount = 0;

bool leakdetected = true;

//Image settings
const size_t MAX_JPG = 40000; 
uint8_t* latestBottleJpg = nullptr;
size_t latestBottleLen = 0;

//Camera operational status
bool cameraHealthy = false;

void IRAM_ATTR handleSensorInterrupt() {
  unsigned long now = millis();
  if (now - lastTriggerTime > DEBOUNCE_DELAY) {
    sensorChanged = true;
    lastTriggerTime = now;
  }
}

// Sends a lightweight 'P' (Ping) command to see if the Camera is alive and responsive
bool checkCameraStatus() {
  // Flush any UART buffer noise
  while (Serial2.available()) Serial2.read();

  Serial.println("[DIAGNOSTIC] Sending Ping ('P') to ESP32-CAM...");
  Serial2.write('P'); // 'P' for Ping

  unsigned long start = millis();
  String response = "";
  
  // Wait up to 1.5 seconds for "PONG" response from camera
  while (millis() - start < 1500) {
    if (Serial2.available()) {
      char c = Serial2.read();
      response += c;
      if (response.indexOf("PONG") != -1) {
        return true;
      }
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
  String camStatusText = cameraHealthy ? "<span style='color:#28a745;'>ONLINE</span>" : "<span style='color:#dc3545;'>OFFLINE (Check Serial Monitor)</span>";
  
  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>body{font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:20px;}"
                "img{width:320px;max-width:100%;border-radius:8px;border:2px solid #444;margin:10px 0;}"
                "button{padding:10px 18px;font-size:16px;border-radius:6px;cursor:pointer;background:#007bff;color:white;border:none;}</style></head><body>"
                "<h2>Smart Water Dispenser Dashboard (ESP32-WROOM)</h2>"
                "<div style='display:flex; justify-content:center; flex-wrap:wrap;'>"
                  "<div class='status-card'>Ambient Humidity<br><b style='font-size:24px; color:#00d2fc;'>" + String(airHumidity, 1) + "%</b></div>"
                  "<div class='status-card'>Temperature<br><b style='font-size:24px; color:#ff7b00;'>" + String(airTemp, 1) + "°C</b></div>"
                "</div>"
                "<p>Camera Status: <b>" + camStatusText + "</b></p>"
                "<p>Sensor Status: <b>" + statusText + "</b> | Total Bottles: <b>" + String(bottleCount) + "</b></p>"
                "<h3>Latest Snapshot</h3>"
                ;

                
  if (latestBottleLen == 0) {
    html += "<p style='color:#888;'>No photo captured yet.</p>";
  } else {
    html += "<img src='/latest.jpg?t=" + String(millis()) + "'><br><br>";
  }
  
  html += "<button onclick=\"location.reload()\">Refresh Dashboard</button>"
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

void setup() {
  Serial.begin(115200);
  delay(500);
  
  Serial2.begin(115200, SERIAL_8N1, 16, 17); // RX2=16, TX2=17
  Serial2.setTimeout(2500);

  latestBottleJpg = (uint8_t*)malloc(MAX_JPG);

  dht.begin(15);
  pinMode(SENSOR_PIN_OBSTACLE, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN_OBSTACLE), handleSensorInterrupt, CHANGE);
  obstaclePresent = (digitalRead(SENSOR_PIN_OBSTACLE) == LOW);

  // --- CONNECT TO WIFI ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected!");
  Serial.print("OPEN DASHBOARD IN BROWSER: http://");
  Serial.println(WiFi.localIP());
  Serial.println("-------------------------------------------------");

  // --- CAMERA DIAGNOSTIC CHECK ---
  Serial.println("[DIAGNOSTIC] Initiating Camera System Check...");
  cameraHealthy = checkCameraStatus();

  if (cameraHealthy) {
    Serial.println("[DIAGNOSTIC SUCCESS] ESP32-CAM is ONLINE and communicating cleanly!");
    Serial.println("Type 'c' and press Enter to snap a manual test picture anytime.");
  } else {
    Serial.println("[DIAGNOSTIC WARNING] ESP32-CAM is OFFLINE!");
    Serial.println("   Please inspect your physical wires:");
    Serial.println("   1. WROOM Pin 17 -> CAM Pin U0R");
    Serial.println("   2. WROOM Pin 16 -> CAM Pin U0T");
    Serial.println("   3. WROOM GND    -> CAM GND (Next to 5V pin)");
    Serial.println("   4. Verify the Camera has red power LEDs lit!");
    Serial.println("   (Camera command 'c' is locked until a connection is established)\n");
  }
  Serial.println("-------------------------------------------------");

  server.on("/", handleRoot);
  server.on("/latest.jpg", handleLatestJpg);
  server.begin();
}

void loop() {
  server.handleClient();

  //Check for leakage
  static unsigned long lastDHTRead = 0;
  if (millis() - lastDHTRead > 2500) {
    lastDHTRead = millis();

    float h = dht.readHumidity();
    float t = dht.readTemperature(); // Read temperature in Celsius

    // If reading succeeded, update global variables
    if (!isnan(h) && !isnan(t)) {
      airHumidity = h;
      airTemp = t;
    } else {
      Serial.println("[WARN] Failed to read from DHT11 sensor!");
    }
  }

  // Monitor Serial input for commands
  if (Serial.available()) {
    char key = Serial.read();
    if (key == 'c' || key == 'C') {
      if (!cameraHealthy) {
        Serial.println("[LOCKED] Cannot capture. Camera system diagnostic failed at boot.");
        Serial.println("Checking camera connection again...");
        cameraHealthy = checkCameraStatus();
        if (cameraHealthy) {
          Serial.println("Camera is now ONLINE! Try pressing 'c' again.");
        } else {
          Serial.println("Camera is still OFFLINE. Check wiring crossed pins (TX->RX, RX->TX).");
        }
      } else {
        Serial.println("\n--- [MANUAL TRIGGER] Capturing snapshot ---");
        requestPhotoFromCam(latestBottleJpg, &latestBottleLen);
      }
    }
  }

  // IR Sensor processing
  if (sensorChanged) {
    sensorChanged = false;
    bool current = (digitalRead(SENSOR_PIN_OBSTACLE) == LOW);
    if (current != obstaclePresent) {
      obstaclePresent = current;
      if (obstaclePresent) {
        bottleCount++;
        Serial.printf("\n[SENSOR] Bottle #%lu Detected! Requesting snapshot...\n", bottleCount);
        if (cameraHealthy) {
          requestPhotoFromCam(latestBottleJpg, &latestBottleLen);
        } else {
          Serial.println("[SENSOR ERROR] Skipping photo trigger - Camera is offline.");
        }
      } else {
        Serial.println("[SENSOR] Bottle Removed.");
      }
    }
  }
}

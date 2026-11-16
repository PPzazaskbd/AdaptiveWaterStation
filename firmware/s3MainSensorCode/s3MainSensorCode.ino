#include <Arduino.h>
#include <DHT.h> // Fixed: Added the missing DHT library header

const int SENSOR_PIN = 4; 
const int DHT_PIN = 5;

// Fixed: Correctly defined the DHT type using the library's definition
#define DHTTYPE DHT11 
DHT dht(DHT_PIN, DHTTYPE);

volatile bool sensorChanged = false; // Sensor change flag
volatile unsigned long lastTriggerTime = 0;
const unsigned long DEBOUNCE_DELAY = 150; // Filter noise

// Obstacle tracking 
bool obstaclePresent = false; 

unsigned long lastDhtReadTime = 0; //last time sensor read
const unsigned long DHT_INTERVAL = 3000; //read humidity every 3 seconds

const float HUMIDITY_ALERT_THRESHOLD = 80.0; //alert if humidity > 80%

// Interrupt Service Routine (ISR)
void IRAM_ATTR handleSensorInterrupt() {
  unsigned long currentTime = millis();
  
  //debouncing
  if (currentTime - lastTriggerTime > DEBOUNCE_DELAY) {
    sensorChanged = true; 
    lastTriggerTime = currentTime;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(1);
  }

  pinMode(SENSOR_PIN, INPUT_PULLUP);
  //listen for any CHANGE instead of just FALLING
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), handleSensorInterrupt, CHANGE);

  dht.begin();

  // (LOW = Obstacle is already there, HIGH = Clear)
  obstaclePresent = (digitalRead(SENSOR_PIN) == LOW);
  
  Serial.println("System initialized. Waiting for obstacles...");
  if (obstaclePresent) {
    Serial.println("STATUS: Obstacle is PRESENT on startup!");
  } else {
    Serial.println("STATUS: Clear path.");
  }
}

void loop() {
  unsigned long currentMillis = millis();

  if (sensorChanged) {
    sensorChanged = false; //reset the interrupt flag

    bool currentPinState = (digitalRead(SENSOR_PIN) == LOW);

    if (currentPinState != obstaclePresent) {
      obstaclePresent = currentPinState;

      if (obstaclePresent) {
        Serial.println("Alert: Obstacle Detected! (Holding active...)");
      } else {
        Serial.println("Clear: Obstacle is GONE. (Releasing active...)");
      }
    }
  }

  // Humidity
  if (currentMillis - lastDhtReadTime >= DHT_INTERVAL) {
    lastDhtReadTime = currentMillis;

    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature(); //celsius

    //check if any reads failed
    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("Warning: Failed to read from DHT sensor on GPIO 5!");
    } else {
      Serial.printf("Enclosure Env: Temp: %.1f °C | Humidity: %.1f %%\n", temperature, humidity);

      //leakage warning
      if (humidity >= HUMIDITY_ALERT_THRESHOLD) {
        Serial.println("CRITICAL WARNING: High moisture inside enclosure! Check for leaks.");
      }
    }
  }
}

#include <Arduino.h>

// Use UART1 to tunnel data to the CAM board on Pins 44 (RX) and 43 (TX)
HardwareSerial CamSerial(1);

void setup() {
  Serial.begin(115200);
  CamSerial.begin(115200, SERIAL_8N1, 44, 43);
}

void loop() {
  // Transfer code data between Computer <--> ESP32-CAM
  while (Serial.available()) {
    CamSerial.write(Serial.read());
  }
  while (CamSerial.available()) {
    Serial.write(CamSerial.read());
  }
}

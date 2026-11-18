#include <Arduino.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// AI-Thinker ESP32-CAM Pinout
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

bool cameraReady = false;

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_RGB565; // For GC2145 (0x2145)
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA; // 320x240
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QQVGA; // 160x120
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  return (esp_camera_init(&config) == ESP_OK);
}

void sendPhotoOverSerial() {
  if (!cameraReady) {
    Serial.write("ERR!", 4);
    return;
  }

  // Flush stale frame and grab fresh frame
  camera_fb_t* stale = esp_camera_fb_get();
  if (stale) esp_camera_fb_return(stale);

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.write("ERR!", 4);
    return;
  }

  uint8_t* jpgBuf = nullptr;
  size_t jpgLen = 0;
  bool ok = frame2jpg(fb, 80, &jpgBuf, &jpgLen);
  esp_camera_fb_return(fb);

  if (!ok || !jpgBuf) {
    Serial.write("ERR!", 4);
    return;
  }

  // Send 4-byte sync header + 4-byte length + raw JPEG bytes
  uint32_t len32 = (uint32_t)jpgLen;
  Serial.write("JPG!", 4);
  Serial.write((uint8_t*)&len32, 4);
  Serial.write(jpgBuf, jpgLen);
  Serial.flush();

  free(jpgBuf);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  setCpuFrequencyMhz(80); // Low ~85mA draw — Wi-Fi stays 100% OFF!
  Serial.begin(115200);   // Matched to WROOM Serial2 (115200 baud)

  cameraReady = initCamera();
}

void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();

    if (cmd == 'P') { 
      // Diagnostic Ping from WROOM
      if (cameraReady) {
        Serial.print("PONG");
      } else {
        Serial.print("CAM_INIT_FAIL");
      }
      Serial.flush();
    } 
    else if (cmd == 'C' || cmd == 'c') { 
      // Capture request from WROOM
      sendPhotoOverSerial();
    }
  }
}

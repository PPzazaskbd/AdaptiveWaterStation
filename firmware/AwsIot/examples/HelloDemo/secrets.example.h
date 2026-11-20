// Copy this file to secrets.h (same folder) and fill it in. secrets.h is in .gitignore: never commit it.
// The private key lets anyone publish as this gateway.
// Same names as AWS's ESP32 tutorial, so its instructions apply as written.
#pragma once
#include <pgmspace.h>

#define THINGNAME "gw-01"  // the IoT thing name; also the MQTT client id

const char WIFI_SSID[] = "your-2.4GHz-wifi";  // ESP32 can't join 5 GHz networks
const char WIFI_PASSWORD[] = "your-password";

// AWS IoT Core, Sydney. Same endpoint as demo/config.js (AWS console: IoT Core -> Settings).
const char AWS_IOT_ENDPOINT[] = "asvgngsn5dm2w-ats.iot.ap-southeast-2.amazonaws.com";

// Amazon Root CA 1 (https://www.amazontrust.com/repository/AmazonRootCA1.pem)
static const char AWS_CERT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
paste AmazonRootCA1.pem here
-----END CERTIFICATE-----
)EOF";

// Device certificate (xxxx-certificate.pem.crt)
static const char AWS_CERT_CRT[] PROGMEM = R"KEY(
-----BEGIN CERTIFICATE-----
paste the device certificate here
-----END CERTIFICATE-----
)KEY";

// Device private key (xxxx-private.pem.key)
static const char AWS_CERT_PRIVATE[] PROGMEM = R"KEY(
-----BEGIN RSA PRIVATE KEY-----
paste the private key here
-----END RSA PRIVATE KEY-----
)KEY";

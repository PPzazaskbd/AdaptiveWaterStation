# AwsIot: connect the gateway to AWS IoT Core

One header, `src/AwsIot.h`. It connects an ESP32 to AWS IoT Core over MQTT with TLS and publishes JSON. It uses the same parts as AWS's tutorial [Building an AWS IoT Core device using AWS Serverless and an ESP32](https://aws.amazon.com/blogs/compute/building-an-aws-iot-core-device-using-aws-serverless-and-an-esp32/): `WiFiClientSecure`, the **MQTT** library by Joel Gaehwiler, and a `secrets.h` with `THINGNAME` and `AWS_CERT_*`. So the tutorial's steps for the AWS console apply as written. On top of the tutorial's code:

- **QoS 1:** `awsIotPublish()` returns `true` only after AWS confirms the message (PUBACK). The tutorial uses QoS 0, which gives no confirmation. PubSubClient can't do QoS 1 at all.
- **Clock:** it sets the clock by SNTP first, so `measured_at` is right.
- **No hanging:** it stops waiting after a timeout instead of looping forever, and `awsIotLoop()` reconnects Wi-Fi and MQTT in the background.

Plan: `SystemArchitecture/communication-plan.md` §1.1 and §1.3. Message format: `SystemArchitecture/message-formats.md` §6.

## Setup

1. **Board package:** Arduino IDE → Boards Manager → **esp32 by Espressif** (tested with 3.3.12).
   - **Library Manager:** install **MQTT by Joel Gaehwiler** (not PubSubClient).
2. **Install the library:** copy the `firmware/AwsIot` folder into `Documents/Arduino/libraries/`. Restart the IDE.
   - PlatformIO: add `lib_extra_dirs = ../firmware` (or copy it into `lib/`).
3. **Secrets:** open *File → Examples → AwsIot → HelloDemo*. Save a copy, then copy `secrets.example.h` to `secrets.h` next to the `.ino` and fill it in:
   - the Wi-Fi name and password (2.4 GHz only);
   - the certificate and private key of the thing `gw-01`, plus Amazon Root CA 1.

   `secrets.h` is in `.gitignore`. **Never commit it.** The key lets anyone publish as our gateway.
4. **Board:** "AI Thinker ESP32-CAM". Upload, then open the Serial Monitor at 115200.

What a good run prints:

```
[aws] Wi-Fi: connecting to "my-hotspot".... ok, IP 192.168.1.23, RSSI -54 dBm
[aws] clock: 2026-11-19T08:15:02.418Z
[aws] MQTT: connecting to asvgngsn5dm2w-ats.iot.ap-southeast-2.amazonaws.com as "gw-01"
[aws] MQTT: connected (#1)
cloud: up
seq 0 delivered (PUBACK in 84 ms): {"v":1,"node_id":"gw-01",...}
```

## The API

| Call | What it does |
|---|---|
| `awsIotBegin(THINGNAME, WIFI_SSID, WIFI_PASSWORD, AWS_IOT_ENDPOINT, AWS_CERT_CA, AWS_CERT_CRT, AWS_CERT_PRIVATE)` | Wi-Fi, then the clock (SNTP), then one MQTT connect attempt. Blocks for a few seconds. Returns `AWSIOT_OK`, or `AWSIOT_NOT_YET` (`awsIotLoop()` keeps trying), or a reason it failed. Pass an `AwsIotConfig` instead for more settings |
| `awsIotLoop()` | **Call it in every `loop()`.** It receives messages, keeps the connection alive, and reconnects every 5 s after a drop |
| `awsIotConnected()` | `true` while connected to AWS |
| `awsIotPublish(topic, json, qos = 1)` | `true` once AWS has the message. Waits for the PUBACK, usually under 0.3 s and at most 1 s. On `false`, keep the message and send it again with the same `seq`. The cloud drops duplicates |
| `awsIotSubscribe(topic)` and `awsIotOnMessage(fn)` | Receives messages on a topic, such as the command topic after 20 Nov. Subscribes again after each reconnect. **Don't publish from inside `fn`:** copy what you need and act on it in `loop()` |
| `awsIotOnConnection(fn)` | `fn(true / false)` when the connection goes up or down |
| `awsIotNowIso(buf, 32)` | Writes the current UTC time as `2026-11-15T18:02:12.071Z`, the format for `measured_at` |

**The gateway also reads the node over UART.** A publish can block for up to 1 s, and a reconnect for 1–3 s. Give the UART room so node lines aren't lost while it blocks: call `Serial2.setRxBufferSize(4096)` before `Serial2.begin(...)`.

## When it doesn't work

| What you see | Likely cause |
|---|---|
| `Wi-Fi ... timed out` | Wrong password, or a 5 GHz network. The ESP32 only joins 2.4 GHz |
| `clock: SNTP timed out` | The network blocks NTP (UDP 123). Try a phone hotspot |
| `MQTT: TLS failed ...` | Wrong endpoint, a certificate or key pasted wrong, a certificate that isn't **Active**, or a certificate that isn't attached to the thing |
| Connects, then disconnects, again and again | The policy doesn't allow the topic, or a **second client is using `gw-01`**: another board or MQTT Explorer. AWS closes the connection, and it gives no error |
| Delivered, but nothing appears on the demo page | The demo dropped the message. `v` must be 1, `node_id` must be a registered device (`gw-01`, `node-01`), and `measured_at` must be correct |

## Status

Compiled on 18 Nov 2026 with Arduino-ESP32 core 3.3.12 and MQTT 2.5.2, for the ESP32-CAM (32% of flash) and the ESP32 Dev Module, with no warnings from this library. A second `.cpp` that includes the header also links. **Not yet tested on a board.** AWS doesn't have the thing `gw-01` or its certificate yet, so step 0 is to create them (plan §1.1).

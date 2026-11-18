# StationCloud: send the station's data to AWS IoT Core

You call functions with plain values (numbers, `true`/`false`, a fault name). The library writes the JSON in the agreed format (`SystemArchitecture/message-formats.md`, version 1), buffers it, and sends it to AWS IoT Core. Underneath it uses `firmware/AwsIot`, which handles Wi-Fi, the clock, TLS and QoS 1.

```cpp
cloudSetClimate(dht.readTemperature(), dht.readHumidity());   // no JSON anywhere
cloudBottlePlaced();
cloudFillEnded(CLOUD_BOTTLE_REMOVED);
cloudFault("CAM_FAULT");
```

## Files

To use the library you only need `StationCloud.h`. Everything else is inside it.

| File | What's in it |
|---|---|
| `src/StationCloud.h` | **The calls and the settings.** This is the only file a sketch includes |
| `src/station_cloud/Types.h` | The `CLOUD_...` values you pass: states, stop reasons, alert and sensor types |
| `src/station_cloud/Connection.cpp` | `cloudBegin` and `cloudLoop`: connecting, the heartbeat, publishing the buffer |
| `src/station_cloud/Readings.cpp` | The `cloudSet...` calls |
| `src/station_cloud/Events.cpp` | Bottles, fills and faults |
| `src/station_cloud/Guards.cpp` | `cloudBottleSeen`, `cloudFaultIf` and the session watchdog |
| `src/station_cloud/Debounce.h` | The two filters they use: one time-based, one count-based |
| `src/station_cloud/Station.h` / `.cpp` | The library's internal state, and how one sample becomes a buffered record |
| `src/station_cloud/Record.h` | One buffered sample: readings, event, diag |
| `src/station_cloud/Queue.h` | The buffer and its drop rules |
| `src/station_cloud/FaultTable.h` | The fault codes: which alert and sensor each one opens, and which id sends it |
| `src/station_cloud/Json.h` | A small JSON writer that never overruns its buffer |
| `src/station_cloud/Format.h` | Record → message JSON, and `measured_at` |
| `test/` | The PC test. `Record`, `Queue`, `FaultTable`, `Debounce`, `Json` and `Format` are plain C++, so it compiles them with `g++` |

## Setup

1. **Libraries:**
   - Library Manager: install **MQTT** by Joel Gaehwiler, and **DHT sensor library** by Adafruit (for the example).
   - Copy both folders, `firmware/AwsIot` and `firmware/StationCloud`, into `Documents/Arduino/libraries/`. Restart the IDE.
2. **Secrets:** open *File → Examples → StationCloud → StationDemo* and save a copy. Copy `secrets.example.h` to `secrets.h` and fill it in: the Wi-Fi name and password (2.4 GHz only), and the certificate and private key of the thing `gw-01`. `secrets.h` is in `.gitignore`. **Never commit it.**
3. **Board:** "ESP32 Dev Module". Upload, then open the Serial Monitor at 115200.

## Adding it to `s3MainSensorCode`

You need five additions. Nothing else in the sketch has to change.

```cpp
#include "secrets.h"            // 1. delete the sketch's own WIFI_SSID / WIFI_PASS lines: secrets.h has them,
#include <StationCloud.h>       //    and its password is called WIFI_PASSWORD

void setup() {
  ...                           //    the sketch's own Wi-Fi loop can go (cloudBegin connects). If it stays,
                                //    change it to WiFi.begin(WIFI_SSID, WIFI_PASSWORD)
  cloudBegin(THINGNAME, WIFI_SSID, WIFI_PASSWORD, AWS_IOT_ENDPOINT,      // 2.
             AWS_CERT_CA, AWS_CERT_CRT, AWS_CERT_PRIVATE);
  cloudSetState(CLOUD_STANDBY);
}

void loop() {
  cloudLoop();                                       // 3. first line of loop()
  ...
  if (!isnan(h) && !isnan(t)) { ... }
  cloudSetClimate(t, h);                             // 4. after the DHT read (NAN is fine)
  ...
  // 5. the IR level on every loop. This replaces the interrupt + sensorChanged logic: that one can miss
  //    the last edge of a bounce and then keep the wrong state until the next change.
  if (cloudBottleSeen(digitalRead(SENSOR_PIN_OBSTACLE) == LOW) == CLOUD_BOTTLE_ARRIVED) {
    bottleCount++;
    if (cameraHealthy) requestPhotoFromCam(latestBottleJpg, &latestBottleLen);
  }
  cloudFaultIf("CAM_FAULT", !cameraHealthy);          // optional: needs 2 bad checks in a row
}
```

## The calls

**Start and run**

| Call | What it does |
|---|---|
| `cloudBegin(THINGNAME, WIFI_SSID, WIFI_PASSWORD, AWS_IOT_ENDPOINT, AWS_CERT_CA, AWS_CERT_CRT, AWS_CERT_PRIVATE)` | Starts Wi-Fi, the clock and MQTT. Usually blocks for a few seconds, at worst about 35 s with no Wi-Fi, never forever. If something isn't ready, `cloudLoop()` keeps retrying. Pass a `CloudConfig` instead to change the settings below |
| `cloudLoop()` | **Call it in every `loop()`.** It keeps the connection up, makes the heartbeat and periodic samples, and sends one buffered message per call |

**Readings.** Pass `NAN` when a sensor fails. It is sent as `null`, never as a made-up 0. Values outside the range the cloud accepts are also sent as `null`, because the cloud would drop the whole message otherwise.

| Call | Sent as | When it goes out |
|---|---|---|
| `cloudSetClimate(tempC, humidityPct)` | gateway `temp_c`, `humidity_pct`. A `NAN` also adds `SHT_FAULT` to `diag.faults` until a good reading | With the next heartbeat |
| `cloudSetState(CLOUD_STANDBY / CLOUD_DISPENSING / CLOUD_ERROR)` | node `state`: the dashboard badge | At once, if it changed |
| `cloudSetValve(open)` | node `valve_open` | At once, if it changed |
| `cloudSetDistance(mm, status)`, `cloudSetMass(grams, raw)`, `cloudSetFlowPulses(n)`, `cloudSetTankLevel(adc)` | the other node readings, for when those sensors exist | With the next node sample, every 1 s |

**Things that happen** (events). They are never dropped from the buffer.

| Call | Sends | Returns |
|---|---|---|
| `cloudBottlePlaced()` | `PRESENCE`, and opens session 1, 2, 3 … | The session number, or 0 if the buffer is full of events |
| `cloudFillEnded(reason, volumeMl = 0, source = CLOUD_VOLUME_TIMER)` | `SUMMARY` for the open session. The duration is measured for you. `reason`: `CLOUD_BOTTLE_REMOVED`, `CLOUD_AI_FULL`, `CLOUD_NOT_A_BOTTLE`, `CLOUD_AI_UNAVAILABLE`, `CLOUD_NO_FLOW`, … | false if no session was open |
| `cloudFault("CODE")` | `FAULT`, once. The alert and sensor type come from the fault table in `message-formats.md`. Calling it again while the fault is still active does nothing, so it's safe to call on every check | true when sent |
| `cloudFaultCleared("CODE")` | `FAULT_CLEARED` | false if the fault wasn't active |
| `cloudSessionOpen()`, `cloudConnected()`, `cloudBuffered()`, `cloudDropped()` | Status, sends nothing | |

**Guards against a state machine going wrong.** Use these when the input is noisy. They read the *level* of the input on every call, not its edges, so one missed interrupt or one odd reading can't leave the state wrong: the next readings correct it.

| Call | What it does |
|---|---|
| `cloudBottleSeen(present)` | Call on every loop with the raw IR reading. Present for 1 s → `cloudBottlePlaced()`. Absent for 0.5 s → `cloudFillEnded(CLOUD_BOTTLE_REMOVED)`, if the session is still open. Shorter bounces are ignored. If the buffer refuses the event, it retries once a second. Returns `CLOUD_BOTTLE_ARRIVED` or `CLOUD_BOTTLE_LEFT` once, at the moment it happens: take the photo on `ARRIVED` |
| `cloudFaultIf("CAM_FAULT", bad, confirm = 2)` | Call with each health-check result. The fault starts only after 2 bad checks in a row and clears only after 2 good ones, so one lost ping doesn't send `FAULT` followed by `FAULT_CLEARED` |
| *Session watchdog* (inside `cloudLoop`) | A session still open after 5 minutes is closed as `TIMEOUT`, so a missed removal can't keep it open forever. A bottle that stays in the bay doesn't open a new session until it has been taken away |

Using these doesn't stop you calling `cloudBottlePlaced()`, `cloudFillEnded()` or `cloudFault()` directly. For example, `cloudFillEnded(CLOUD_AI_FULL)` when the AI says the bottle is full; the guard then only reacts when the bottle leaves.

**Settings (`CloudConfig`)**

| Setting | Default | |
|---|---|---|
| `topic` | `water/demo/telemetry` | The 20 Nov demo. Later `water/gw-01/telemetry` |
| `nodeId` | `node-01` | The id for bottle and valve data. The gateway id is `THINGNAME` (`gw-01`). The demo stores only these two |
| `heartbeatMs` | 15000 | Use 2000 for the video, so the page updates quickly (plan §1.2, trap 3) |
| `nodeSampleMs` | 1000 | Plain node samples, once any node value has been set. 0 = only on changes and events |
| `bufferRecords` | 60 | About 230 bytes each, roughly 14 KB in all |
| `bottleOnMs` / `bottleOffMs` | 1000 / 500 | How long the IR must say present / absent before `cloudBottleSeen` acts |
| `maxSessionMs` | 300000 | The session watchdog (5 min). 0 = off |

## What it does underneath (`communication-plan.md` §4.5)

- **Nothing waits for the network.** Every call stores a record in RAM and returns at once.
- **Delivery is confirmed.** `cloudLoop()` publishes the oldest record at QoS 1 and removes it only after AWS's PUBACK. On failure it keeps the record, raises `attempt` and sends it again. The cloud drops duplicates by `(node_id, boot_id, seq)`.
- **Offline,** records wait in the buffer. After a reconnect, a fresh heartbeat goes first, then the backlog, oldest first. Each record keeps the time it was taken (`measured_at`), even if it was taken before the clock was set.
- **Buffer full:** the oldest record without an event is dropped, and counted in the gateway's `diag.dropped`. If the buffer holds only events, the new call returns false.
- **Two ids from one board.**
  - `gw-01` sends the heartbeat (temperature, humidity, connection state).
  - `node-01` sends the bottle, the state and the sessions.
  - Each has its own `seq`, which starts at 0 at every boot. A new random `boot_id` is made at each boot, and the first message of each id carries `reset_reason`.

## Tested

- `node firmware/StationCloud/test/run.mjs`, run from the repo root (needs `g++` and Node). The C++ checks cover the buffer's drop and order rules, the timestamps, the fault codes, the active-fault list, buffer overrun and both filters. The filter tests include IR bouncing, a stray reading mid-fill, the missed-edge case and `millis()` wrapping around. **Every message type passes `demo/contract.js`,** the checker the cloud demo uses: heartbeat, failed sensor, full sample, `PRESENCE`, both kinds of `SUMMARY`, `FAULT`, `FAULT_CLEARED`.
- The example compiles for "ESP32 Dev Module" with Arduino-ESP32 3.3.12 and MQTT 2.5.2, with no warnings from this library. It was compiled against a stand-in DHT header. The sketch uses 79 % of flash. A copy of `s3MainSensorCode` with the five additions also compiles.
- **Not yet run on a board.** AWS doesn't have the thing `gw-01` or its certificate yet (`communication-plan.md` §1.1).

## Not in it yet

- **Dashboard commands** (`water/gw-01/command`, messages 14–15) and **`AI_RESULT`** (message 13). The demo would reject both, and nothing on the cloud side sends commands yet. They come after 20 Nov.
- **The node ↔ gateway UART link** (plan §3.1). It isn't needed while one board does both jobs. Serial2 carries the camera's own `P`/`C`/`JPG!` protocol.

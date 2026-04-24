# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

## Build & Flash Commands

```bash
# Build firmware
pio run -e Espectrografo_AS7265X

# Upload to device
pio run -e Espectrografo_AS7265X --target upload

# Open serial monitor (115200 baud)
pio device monitor --baud 115200

# Build + upload + monitor
pio run -e Espectrografo_AS7265X --target upload && pio device monitor --baud 115200

# Clean build artifacts
pio run --target clean
```

No automated tests — this is embedded firmware. Verification is via serial monitor and the web UI.

## Project Summary

ESP32 portable water-analysis spectrograph using the AS7265X 18-channel spectral sensor (410–940 nm). The system runs a state machine in `loop()`, with a non-blocking HTTP web server (ESPAsyncWebServer) for the HMI. Data is saved to a microSD card in CSV format.

## Architecture Overview

### State Machine Flow

```
IDLE → CALIBRATION → WAIT_CONFIRMATION → MEASUREMENT → VALIDATION → SAVE_DECISION → IDLE
IDLE → LIVE_MONITOR → IDLE
```

- `g_stateMachine.tick()` called every 10 ms from `loop()`
- State transitions via `requestTransition()` — applied on next tick
- Web API handlers run on Core 0 (AsyncTCP ISR context) — they only call `requestTransition()`, never block
- Calibration and measurement engines are polled in `loop()` only when in the matching state

#### Detailed Workflow

```
[IDLE]
  ↓ user clicks "1. Start Calibration"
  POST /api/calibrate → requestTransition(CALIBRATION)

[CALIBRATION]
  g_calibration.tick() collects 5 samples × 500 ms = ~4.6 s
  (each takeMeasurement() blocks ~420 ms; 500 ms gap between triggers)
  g_calibration._done = true
  StateMachine::tick() auto-transitions → exitState(CALIBRATION)
    → g_sdLogger.saveCalibration(...)
    → g_calibration.clearDoneFlag()
  → enterState(WAIT_CONFIRMATION)

[WAIT_CONFIRMATION]
  User physically inserts sample cuvette
  POST /api/confirm → requestTransition(MEASUREMENT)

[MEASUREMENT]
  g_measurementEngine.tick() collects N spectra at 500 ms intervals
  StateMachine::tick() auto-transitions → VALIDATION

[VALIDATION]
  POST /api/accept → requestTransition(SAVE_DECISION)

[SAVE_DECISION]
  POST /api/save → g_sdLogger.saveExperiment(...) → IDLE
  POST /api/discard → IDLE

[LIVE_MONITOR]
  Entered from IDLE via POST /api/monitor/start
  loop() reads sensor continuously into g_liveBuf[18]
  GET /api/monitor returns live channel data
  POST /api/monitor/stop → IDLE
```

### Module Responsibilities

| Module | File | Responsibility |
|---|---|---|
| State Machine | `src/core/state_machine.cpp` | `SystemState` enum, transitions, `enterState`/`exitState` hooks |
| Sensor Driver | `src/sensors/as7265x_driver.cpp` | Wraps SparkFun AS7265X lib; applies `SensorConfig`; reads 18 channels |
| Calibration | `src/acquisition/calibration.cpp` | 5-sample blank reference average; produces `CalibrationData` with offset[18] |
| Measurement Engine | `src/acquisition/measurement_engine.cpp` | N sequential readings at 500 ms intervals; stores `float spectra[20][18]` |
| SD Logger | `src/storage/sd_logger.cpp` | VSPI (MOSI=23, MISO=19, SCK=18, CS=5); **FILE_APPEND** to `/spectra.csv` |
| Web Server | `src/web/web_server.cpp` | WiFi AP + scan + STA connection; HTTP server lifecycle |
| API Routes | `src/web/api_routes.cpp` | REST endpoints; all responses include `Cache-Control: no-store` |
| Embedded Frontend | `src/ui/html_content.h` | Single-page HTML/CSS/JS in PROGMEM; WiFi panel overlay; live chart |

### REST API Endpoints

| Endpoint | Method | State Guard | Purpose |
|---|---|---|---|
| `/api/status` | GET | — | State, sensorReady, sdReady, calValid, measCount/Target |
| `/api/config` | GET | — | Current sensor configuration |
| `/api/config` | POST | IDLE only | Set gain, integration, LEDs, N, expId |
| `/api/calibrate` | POST | IDLE only | Begin blank reference calibration |
| `/api/confirm` | POST | WAIT_CONFIRMATION | Sample inserted, proceed to measurement |
| `/api/measure` | POST | IDLE or WAIT_CONFIRMATION | Start measurement directly |
| `/api/spectra` | GET | — | All acquired spectra + wavelengths |
| `/api/calibration` | GET | — | Current calibration offsets |
| `/api/accept` | POST | VALIDATION | Proceed to save dialog |
| `/api/save` | POST | SAVE_DECISION | Write experiment to SD, return to IDLE |
| `/api/discard` | POST | SAVE_DECISION | Discard data, return to IDLE |
| `/api/monitor/start` | POST | IDLE | Enter live monitor mode |
| `/api/monitor/stop` | POST | LIVE_MONITOR | Exit live monitor |
| `/api/monitor` | GET | — | Live 18-channel reading (`g_liveBuf`) |
| `/api/wifi` | GET | — | WiFi STA connection status string |
| `/api/wifi` | POST | — | Connect: `{"ssid":"...","password":"..."}` |
| `/api/wifi/scan` | POST | — | Trigger network scan (non-blocking, driven from loop) |
| `/api/wifi/scan` | GET | — | Scan status + cached results |

## WiFi Architecture

### Access Point

Default mode on boot. SSID: `Espectrografo-AP`, password: `esp32spectro`, IP: `192.168.4.1`.

### Network Scan (ScanStep state machine in `web_server.cpp`)

The ESP32 has a single radio — scanning while serving AP clients is unreliable. The scan runs as a state machine driven from `webServerLoop()` on Core 1, fully decoupled from the async web handler on Core 0.

```
IDLE → REQUESTED → RADIO_OFF (500ms) → STA_INIT (1000ms) → SCANNING (blocking ~4s)
     → RESTORING_AP (1500ms) → DONE
```

- During SCANNING: HTTP server is stopped (`g_httpServer.end()`), AP torn down, radio in WIFI_STA
- Scan results are copied into `ScanNet s_scanResults[20]` (fixed C structs, NOT String/heap) **before** any mode change, so driver memory invalidation on mode switch cannot corrupt the list
- After scan: `WiFi.disconnect(true)` → `WIFI_AP` → `softAP()` → 1500ms settle → `g_httpServer.begin()` → `s_hasResults = true`
- `wifiScanResultsJson()` builds JSON on demand from the fixed struct array

### STA Connection

Triggered by POST `/api/wifi`. Runs inline in `webServerLoop()`:

1. `g_httpServer.end()` + AP torn down → `WIFI_STA` + 1000ms settle → `WiFi.begin()`
2. Polls `WiFi.status()` every loop; waits up to **15 seconds** (never bails early on `WL_CONNECT_FAILED` — the driver transiently passes through that state)
3. On success: `g_httpServer.begin()` on STA IP + NTP sync (`configTime()`)
4. On timeout: `restoreAP()` — `WIFI_AP` + `softAP()` + 500ms delay + `g_httpServer.begin()`

Serial output during connection:
```
[WiFi] Dropping AP, connecting to 'SSID'...
[WiFi] Attempting connection (up to 15s)...
[WiFi] Connected! IP: 192.168.x.x — AP torn down, HTTP re-listening
[WiFi] NTP sync requested
```
or on failure:
```
[WiFi] 15s timeout — last status=N — restoring AP
[WiFi] Restoring AP...
[WiFi] AP back at 192.168.4.1
```

## Key Data Types

```cpp
struct SensorConfig {
    SensorGain      gain              = GAIN_16X;   // 0=1x,1=4x,2=16x,3=64x
    uint8_t         integrationCycles = 50;         // ×2.8 ms/cycle per die
    MeasurementMode mode              = MODE_3;     // 3 = one-shot all 18 ch
    uint8_t         ledWhiteCurrent   = 12;         // mA: 12,25,50,100
    uint8_t         ledIrCurrent      = 12;
    uint8_t         ledUvCurrent      = 12;
    bool            ledWhiteEnabled   = false;
    bool            ledIrEnabled      = false;
    bool            ledUvEnabled      = false;
};

struct CalibrationData {
    bool  valid;
    float offset[18];    // blank subtracted from measurements
    float reference[18]; // raw blank average (logged to SD)
};

struct Experiment {
    char            experiment_id[32];
    uint32_t        timestamp;         // millis() at start
    int             num_measurements;  // target N (1–20)
    SensorConfig    sensor_cfg;        // snapshot at start
    CalibrationData calibration;       // snapshot at start
    float           spectra[20][18];   // calibrated readings
    int             count;             // spectra actually stored
};
```

## CSV Format (`/spectra.csv`)

One row per measurement; all experiments share the same file (FILE_APPEND, never FILE_WRITE).

```
date,exp_id,meas_idx,gain,int_cycles,led_white_ma,led_ir_ma,led_uv_ma,
  led_white,led_ir,led_uv,
  cal_ch1..cal_ch18,ch1..ch18
```

- `date`: ISO `2024-05-01 12:34:56` when NTP sync has occurred; `boot+67s` otherwise
- `gain`: human-readable string `"1x"`, `"4x"`, `"16x"`, `"64x"`
- `led_white/ir/uv`: `"ON"` / `"OFF"`

Calibration file: `/calibration.csv` (offset[18] per row, written on each CALIBRATION exit).

## Hardware Pins

| Function | GPIO |
|---|---|
| I2C SDA (AS7265X) | 21 |
| I2C SCL (AS7265X) | 22 |
| SD MOSI | 23 |
| SD MISO | 19 |
| SD SCK | 18 |
| SD CS | 5 |

I2C: 400 kHz. SD SPI: 4 MHz (VSPI).

## Sensor Wavelengths (AS7265X)

18 channels, 410–940 nm:
```
Index  1    2    3    4    5    6    7    8    9   10   11   12   13   14   15   16   17   18
   nm 410  435  460  485  510  535  560  585  610  645  680  705  730  760  810  860  900  940
```

## Key Timing Constants

| Constant | Value | Location |
|---|---|---|
| `CAL_SAMPLE_INTERVAL_MS` | 500 ms | `calibration.cpp` |
| `READ_INTERVAL_MS` | 500 ms | `measurement_engine.h` |
| Sensor blocking read (Mode 3, 50 cycles) | ~420 ms | driver |
| State machine tick | 10 ms | `main.cpp` |
| WiFi scan radio settle (RADIO_OFF) | 500 ms | `web_server.cpp` |
| WiFi scan STA init settle | 1000 ms | `web_server.cpp` |
| WiFi scan AP restore settle | 1500 ms | `web_server.cpp` |
| STA connection settle (before WiFi.begin) | 1000 ms | `web_server.cpp` |
| STA connection timeout | 15000 ms | `web_server.cpp` |
| `CALIBRATION_AVERAGES` | 5 | `calibration.h` |
| `MAX_MEASUREMENTS` | 20 | `measurement_engine.h` |
| Serial baud | 115200 | `platformio.ini` |

## Dependencies

Board: `esp32doit-devkit-v1`, framework: `arduino`, partition: `min_spiffs.csv`

| Library | Version | Use |
|---|---|---|
| `sparkfun/SparkFun Spectral Triad AS7265X` | ^1.0.5 | Sensor I2C driver |
| `esphome/ESPAsyncWebServer-esphome` | ^3.2.2 | Non-blocking HTTP server |
| `esphome/AsyncTCP-esphome` | ^2.1.4 | TCP foundation |
| `bblanchon/ArduinoJson` | ^7.2.1 | JSON in API routes |
| `arduino-libraries/SD` | — | MicroSD via SPI |

Build flags: `-DCORE_DEBUG_LEVEL=0 -DBOARD_HAS_PSRAM`

## Known Bugs Fixed

### SD data loss (`FILE_WRITE` truncation)
`SD.open(LOG_FILE, FILE_WRITE)` on ESP32 maps to `"w"` (truncate), not append. Fixed to `FILE_APPEND`.

### Config zeroed in CSV
`resetExperiment()` called `memset(0)` after `configure(cfg)`, wiping the just-set config. Fixed by preserving `sensor_cfg` across the memset.

### Date shows raw milliseconds
`exp.timestamp = millis()` is raw ms since boot. Fixed with NTP + `time()` + `strftime()`, fallback to `boot+Xs`.

### Gain/mode saved as raw enum integers
Fixed with `gainStr()` helper returning `"1x"/"4x"/"16x"/"64x"`.

### WiFi scan finds 0 networks in AP mode
Single 2.4 GHz radio cannot scan while serving AP clients. Fixed by full radio cycle: AP off → STA → scan → AP on.

### Scan results lost after mode switch
`WiFi.SSID(i)` / `WiFi.RSSI(i)` are invalidated when the driver mode changes. Fixed by copying into `ScanNet s_scanResults[20]` (fixed C structs) before any mode change, then calling `WiFi.scanDelete()`.

### STA connection fails too quickly
`WL_CONNECT_FAILED` fires transiently during DHCP negotiation. Fixed by removing early-exit on `WL_CONNECT_FAILED` — only the 15 s timeout terminates the attempt.

### `netstack cb reg failed with 12308`
Caused by cycling `WIFI_OFF → WIFI_STA → WIFI_OFF → WIFI_AP` (double netif init). Fixed by going directly `STA → AP` after scan, skipping the second `WIFI_OFF`.

## Backend Stack (Docker)

A four-service Compose stack under [docker-compose.yml](docker-compose.yml) provides storage, a REST API, a remote web UI, and an MQTT ingestion path. Bring up / tear down:

```bash
docker compose up -d            # start
docker compose up -d --build    # rebuild after server/ changes
docker compose down             # stop (keep data)
docker compose down -v          # stop + wipe mysql_data volume (re-runs init.sql)
```

### Services

| Service | Container | Image / Build | Ports | Role |
|---|---|---|---|---|
| `mosquitto` | `espectrografo_mqtt` | `eclipse-mosquitto:2.0` | 1883, 9001 | MQTT broker; listener 1883 TCP + 9001 WebSocket |
| `mysql` | `espectrografo_db` | `mysql:8.4` | 3306 | Persistence; init.sql on first boot; healthcheck gates dependents |
| `flask` | `espectrografo_flask` | [docker/Dockerfile.flask](docker/Dockerfile.flask) | 5000 | REST API + serves `control.html` at `/` |
| `mqtt_bridge` | `espectrografo_bridge` | [docker/Dockerfile.mqtt_bridge](docker/Dockerfile.mqtt_bridge) | — | Subscribes to MQTT, writes into MySQL |

Inside the Compose network, services reach each other by service name (`mosquitto`, `mysql`). From the host browser, use `localhost:9001` (WS) / `localhost:5000` (HTTP).

### Config files

- [docker/mosquitto.conf](docker/mosquitto.conf) — two listeners, `allow_anonymous true`. **Must be saved as UTF-8 without BOM** — a BOM makes mosquitto fail with `Unknown configuration variable "listener"`.
- [docker/.env](docker/.env) — holds `MYSQL_ROOT_PASSWORD`, `MYSQL_PASSWORD` (read by Python code), `MYSQL_DATABASE`, `MQTT_BROKER`, `MQTT_PORT`. Both `MYSQL_ROOT_PASSWORD` and `MYSQL_PASSWORD` must match; the first initializes MySQL, the second is what `app.py`/`mqtt_to_db.py` read.
- [docker/mysql-init/init.sql](docker/mysql-init/init.sql) — creates `experimentos`, `mediciones`, `calibraciones`. Only runs when `mysql_data` volume is empty.

### MySQL schema (`espectrografo` database)

- `experimentos(exp_id UNIQUE, timestamp_ms, num_measurements, gain, mode, int_cycles, led_*_ma, cal_valid)` — one row per experiment
- `mediciones(exp_id FK, meas_index, ch1..ch18)` — one row per measurement
- `calibraciones(exp_id FK, ch1..ch18)` — one row per experiment (blank offsets)

### MQTT → DB ingestion — [server/mqtt_to_db.py](server/mqtt_to_db.py)

Subscribes on `mosquitto:1883` to:

| Topic | Handling |
|---|---|
| `esp32/data/upload` | Full experiment JSON → `INSERT IGNORE` into the three tables |
| `esp32/data/spectra` | Same handling as `upload` (alias path) |
| `esp32/data/status` | Logged heartbeat (state + rssi), not persisted |

Uses `paho-mqtt` with `CallbackAPIVersion.VERSION2`. Reads `MQTT_BROKER`, `MQTT_PORT`, `MYSQL_*` from env.

### REST API — [server/app.py](server/app.py) (Flask on port 5000)

CORS open (`Access-Control-Allow-Origin: *`).

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | Serves `control.html` |
| `/history/experiments?limit=&offset=` | GET | Paginated experiment list (max limit 500) |
| `/history/spectra?exp_id=` | GET | All spectra + calibration offsets for one experiment |
| `/history/export/csv?exp_id=` or `?all=true` | GET | CSV download (single experiment or full dataset) |
| `/history/export/json?exp_id=` or `?all=true` | GET | JSON download |
| `/verify?exp_id=&expected=N` | GET | Confirms `mediciones` row count ≥ N — used by the ESP32 to decide whether to purge local SD data |

### Remote UI — [server/control.html](server/control.html)

Single-page HTML served by Flask. Connects to the MQTT broker over WebSockets via `paho-mqtt.js` (`MQTT_HOST`/`MQTT_PORT` constants at [control.html:253-254](server/control.html#L253-L254) — change `MQTT_HOST` from `localhost` if serving remotely). Sends control commands and receives live channel data by subscribing to ESP32 topics.

### Gotchas observed during setup

- **BOM in mosquitto.conf** — any editor that writes UTF-8 with BOM (e.g., Windows Notepad) breaks the broker. Re-save as UTF-8 (no BOM).
- **Password env mismatch** — code reads `MYSQL_PASSWORD`; MySQL image only honors `MYSQL_ROOT_PASSWORD`. Both must be present in `.env` and must match. Changing `.env` after the data volume exists has no effect — use `docker compose down -v` to re-init.
- **WebSocket listener** — browser MQTT requires `protocol websockets` on a distinct listener from the 1883 TCP one.
- **`MQTT_PORT` default** — `mqtt_to_db.py` defaults to `1884`; docker-compose overrides it via env to `1883`. Don't rely on the default.

## Future Work

- **NTP**: already wired (`configTime()` on STA connect); date column in CSV will auto-correct once ESP32 reaches internet.
- **Experiment ID auto-increment**: handled in JavaScript after each successful save — no backend counter needed.

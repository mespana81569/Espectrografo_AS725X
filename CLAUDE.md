# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash Commands

```bash
# Build firmware
pio run -e Espectrografo_AS7265X

# Upload to device
pio run -e Espectrografo_AS7265X --target upload

# Open serial monitor (115200 baud)
pio device monitor --baud 115200

# Build + upload + monitor in one step
pio run -e Espectrografo_AS7265X --target upload && pio device monitor --baud 115200

# Clean build artifacts
pio run --target clean
```

There are no automated tests — this is a hardware firmware project. Verification is done via serial monitor output and the web UI.

## Architecture Overview

This is an ESP32 firmware for a portable water-analysis spectrograph using the AS7265X 18-channel spectral sensor (410–940 nm). The system runs a state machine driven in `loop()`, with a non-blocking HTTP web server (ESPAsyncWebServer) for the HMI.

### State Machine Flow

```
IDLE → CALIBRATION → WAIT_CONFIRMATION → MEASUREMENT → VALIDATION → SAVE_DECISION → IDLE
IDLE → LIVE_MONITOR → IDLE
```

- `g_stateMachine.tick()` is called every 10 ms from `loop()`
- State transitions are requested via `requestTransition()` — they are applied on the next tick
- Web API callbacks (ISR context, Core 0) call `requestTransition()` — never block
- Calibration and measurement engines are polled via their own `tick()` inside `loop()` only when the state machine is in the matching state

#### Detailed Workflow

```
[IDLE]
  ↓ user clicks "1. Start Calibration"
  POST /api/calibrate → requestTransition(CALIBRATION)  [thread-safe]

[CALIBRATION]
  g_calibration.tick() collects 5 samples × 500 ms = ~4.6 s
  (each takeMeasurement() blocks ~420 ms; 500 ms gap between triggers)
  g_calibration._done = true
  StateMachine::tick() auto-transitions → exitState(CALIBRATION)
    → g_sdLogger.saveCalibration(...)
    → g_calibration.clearDoneFlag()
  → enterState(WAIT_CONFIRMATION)

[WAIT_CONFIRMATION]
  UI: btnConfirm enabled, all others disabled
  User physically inserts sample cuvette
  POST /api/confirm → requestTransition(MEASUREMENT)  [thread-safe]

[MEASUREMENT]
  g_measurementEngine.tick() collects N spectra at 500 ms intervals
  StateMachine::tick() auto-transitions → VALIDATION

[VALIDATION]
  UI: btnAccept enabled
  POST /api/accept → requestTransition(SAVE_DECISION)

[SAVE_DECISION]
  UI: btnSave + btnDiscard enabled
  POST /api/save → g_sdLogger.saveExperiment(...) → IDLE
  POST /api/discard → IDLE

[LIVE_MONITOR]
  Entered from IDLE via POST /api/monitor/start
  loop() reads sensor continuously, populates g_liveBuf[18]
  GET /api/monitor returns live channel data
  POST /api/monitor/stop → IDLE
```

### Module Responsibilities

| Module | File | Responsibility |
|---|---|---|
| State Machine | `src/core/state_machine.cpp` | Owns `SystemState` enum, drives transitions, calls `enterState`/`exitState` hooks |
| Sensor Driver | `src/sensors/as7265x_driver.cpp` | Wraps SparkFun AS7265X lib; applies `SensorConfig` (gain, int cycles, mode, LED mA); reads all 18 channels |
| Calibration | `src/acquisition/calibration.cpp` | Collects 5-sample blank reference average; produces `CalibrationData` with offset[18]; `applyOffset()` subtracts from spectra |
| Measurement Engine | `src/acquisition/measurement_engine.cpp` | Runs N sequential readings at 500 ms intervals; stores `float spectra[20][18]` in `Experiment` struct in RAM |
| SD Logger | `src/storage/sd_logger.cpp` | VSPI (MOSI=23, MISO=19, SCK=18, CS=5); appends CSV rows to `/spectra.csv`; creates header on first write |
| Web Server | `src/web/web_server.cpp` | WiFi AP mode (SSID: `Espectrografo-AP`, pass: `esp32spectro`, IP: `192.168.4.1`); serves embedded HTML from PROGMEM; optional STA client mode |
| API Routes | `src/web/api_routes.cpp` | REST endpoints on ESP32; publishes state/spectra/status to MQTT topics |
| Embedded Frontend | `src/ui/html_content.h` | Single-page HTML/CSS/JS in `PROGMEM` string; direct API polling (no MQTT); last 3 spectra chart; WiFi config overlay |
| MQTT Bridge | `mqtt_to_db.py` | Subscribes to `esp32/data/*`, stores experiments/calibrations/measurements in MySQL database |
| Remote Web Control | `control.html` | Standalone single-file HTML served from university server; Paho MQTT over WebSockets; real-time control and historical data viewing |
| Flask Backend | `app.py` | Serves `control.html`, /history API routes (experiments, spectra, export); MySQL backend; CORS enabled |

### REST API Endpoints (ESP32)

| Endpoint | Method | State Guard | Purpose |
|---|---|---|---|
| `/api/status` | GET | — | State, sensorReady, sdReady, calValid, measCount/Target |
| `/api/config` | GET | — | Current sensor configuration |
| `/api/config` | POST | IDLE only | Set gain, integration, LEDs, N, expId |
| `/api/calibrate` | POST | IDLE only | Begin blank reference calibration |
| `/api/confirm` | POST | WAIT_CONFIRMATION only | Sample inserted, proceed to measurement |
| `/api/measure` | POST | IDLE or WAIT_CONFIRMATION | Start measurement acquisition directly |
| `/api/spectra` | GET | — | All acquired spectra + wavelengths for current experiment |
| `/api/calibration` | GET | — | Current calibration offsets |
| `/api/accept` | POST | VALIDATION only | Proceed to save dialog |
| `/api/save` | POST | SAVE_DECISION only | Write experiment to SD, return to IDLE |
| `/api/discard` | POST | SAVE_DECISION only | Discard data, return to IDLE |
| `/api/monitor/start` | POST | IDLE only | Enter live monitor mode |
| `/api/monitor/stop` | POST | LIVE_MONITOR only | Exit live monitor |
| `/api/monitor` | GET | — | Live 18-channel reading (`g_liveBuf`) |
| `/api/wifi` | GET | — | WiFi STA connection status |
| `/api/wifi` | POST | — | Connect to external WiFi `{"ssid":"...","password":"..."}` |
| `/api/wifi/scan` | POST | — | Trigger network scan |
| `/api/wifi/scan` | GET | — | Scan status + cached results |

**MQTT Publishing** (when in STA WiFi mode):
- On state transitions: publish to `esp32/data/state`
- On timer (~5s): publish status to `esp32/data/status` (heartbeat)
- After measurement complete: publish spectra to `esp32/data/spectra`
- After save to SD: publish to `esp32/data/upload`
- During live monitor: publish live channels to `esp32/data/monitor`

**MQTT Subscription** (when in STA WiFi mode):
- Subscribe to `esp32/cmd/#` and handle commands published by remote clients

### Key Data Types

```cpp
// Sensor configuration — defaults shown
struct SensorConfig {
    SensorGain      gain              = GAIN_16X;   // 0=1x, 1=4x, 2=16x, 3=64x
    uint8_t         integrationCycles = 50;         // ×2.8 ms/cycle → ~140 ms
    MeasurementMode mode              = MODE_3;     // 3 = one-shot all 18 channels
    uint8_t         ledWhiteCurrent   = 12;         // mA: 12, 25, 50, 100
    uint8_t         ledIrCurrent      = 12;
    uint8_t         ledUvCurrent      = 12;
    bool            ledWhiteEnabled   = false;
    bool            ledIrEnabled      = false;
    bool            ledUvEnabled      = false;
};

// Blank reference produced by Calibration engine
struct CalibrationData {
    bool  valid;
    float offset[18];    // blank reading subtracted from measurements
    float reference[18]; // raw blank average (logged to SD)
};

// One complete acquisition session (up to 20 readings)
struct Experiment {
    char            experiment_id[32];
    uint32_t        timestamp;          // millis() at start
    int             num_measurements;   // target N (1–20)
    SensorConfig    sensor_cfg;         // snapshot at start
    CalibrationData calibration;        // snapshot at start
    float           spectra[20][18];    // calibrated readings
    int             count;              // spectra actually stored
};
```

### Global Singletons

All modules expose a single global instance (`g_stateMachine`, `g_sensorDriver`, `g_calibration`, `g_measurementEngine`, `g_sdLogger`). These are safe to access from web callbacks only for reads or `requestTransition()` — never call blocking sensor/SD operations from a web handler.

Live monitor buffer is `g_liveBuf[18]` (owned by `main.cpp`), guarded by `volatile bool g_liveReady`.

### CSV Format

SD card files: `/spectra.csv` (measurements) and `/calibration.csv` (blank references).

```
date_ms,exp_id,meas_idx,gain,mode,int_cycles,led_white_ma,led_ir_ma,led_uv_ma,
  cal_ch1..cal_ch18,ch1..ch18
```

One row per individual measurement; multiple rows per experiment share the same `exp_id`.

## Hardware Pins

| Function | GPIO |
|---|---|
| I2C SDA (AS7265X) | 21 (default Wire) |
| I2C SCL (AS7265X) | 22 (default Wire) |
| SD MOSI | 23 |
| SD MISO | 19 |
| SD SCK | 18 |
| SD CS | 5 |

I2C clock: 400 kHz. SD SPI clock: 4 MHz (VSPI).

## Sensor Wavelengths (AS7265X)

18 channels, 410–940 nm:

```
Index  1   2    3    4    5    6    7    8    9    10   11   12   13   14   15   16   17   18
   nm 410 435 460 485 510 535 560 585 610 645 680 705 730 760 810 860 900 940
```

Used in all spectra arrays, charts, and CSV/JSON exports.

## Dependencies

Managed via `platformio.ini` (board: `esp32doit-devkit-v1`, framework: `arduino`, partition: `min_spiffs.csv`):

| Library | Version | Use |
|---|---|---|
| `sparkfun/SparkFun Spectral Triad AS7265X` | ^1.0.5 | Sensor I2C driver |
| `esphome/ESPAsyncWebServer-esphome` | ^3.2.2 | Non-blocking HTTP server |
| `esphome/AsyncTCP-esphome` | ^2.1.4 | TCP foundation for async server |
| `bblanchon/ArduinoJson` | ^7.2.1 | JSON serialization in API routes |
| `arduino-libraries/SD` | — | MicroSD via SPI |

Build flags: `-DCORE_DEBUG_LEVEL=0 -DBOARD_HAS_PSRAM`.

## Key Timing Constants

| Constant | Value | Location |
|---|---|---|
| `CAL_SAMPLE_INTERVAL_MS` | 500 ms | `calibration.cpp` |
| `READ_INTERVAL_MS` | 500 ms | `measurement_engine.h` |
| Sensor blocking read (Mode 3, 50 cycles) | ~420 ms | driver |
| State machine tick | 10 ms | `main.cpp` |
| Serial baud | 115200 | `platformio.ini` |
| `CALIBRATION_AVERAGES` | 5 | `calibration.h` |
| `MAX_MEASUREMENTS` | 20 | `measurement_engine.h` |

Total calibration time: ~4.6 s (5 × (420 ms blocking + 500 ms gap)).
Total measurement time per experiment: N × ~920 ms.

## Remote Control System

### Architecture

Two frontends connect to the ESP32:

1. **Embedded UI** (`src/ui/html_content.h`) — served from ESP32 via HTTP on WiFi AP mode
   - Direct REST polling of `/api/*` endpoints
   - Stateful control: buttons, pipeline, config, live monitor
   - Last 3 spectra chart

2. **Remote Web Control** (`control.html`) — served from external Flask server
   - Connects to MQTT broker (not directly to ESP32 HTTP)
   - Real-time updates via MQTT messages
   - Access to historical experiments from MySQL database
   - Three tabs: live control panel + experiments table + spectra viewer + export

### MQTT Message Flow

**ESP32 → Broker** (ESP publishes after state changes or on schedule):
- `esp32/data/state` → `{state, timestamp_ms, measCount, measTarget, ...}` (on transition)
- `esp32/data/status` → `{state, sensorReady, sdReady, calValid, rssi, heap}` (heartbeat)
- `esp32/data/spectra` → `{exp_id, spectra:[[18]×N], offsets:[18], wavelengths, count, timestamp_ms}` (after measurement)
- `esp32/data/upload` → `{exp_id, timestamp_ms, ...}` (on save to SD)
- `esp32/data/monitor` → `{ready, ch:[18], wl:[18]}` (live mode only)

**Broker ← Remote Client** (control.html publishes commands):
- `esp32/cmd/calibrate` → `""` (empty payload)
- `esp32/cmd/confirm` → `""`
- `esp32/cmd/measure` → `""`
- `esp32/cmd/accept` → `""`
- `esp32/cmd/save` → `""`
- `esp32/cmd/discard` → `""`
- `esp32/cmd/monitor/start` → `""`
- `esp32/cmd/monitor/stop` → `""`
- `esp32/cmd/config` → `{gain,integrationCycles,ledWhiteEnabled,ledWhiteCurrent,...}`

**Bridge** (`mqtt_to_db.py`):
- Listens on `esp32/data/*`
- Inserts into MySQL tables: `experimentos`, `calibraciones`, `mediciones`
- One-shot inserts with `INSERT IGNORE` (prevents duplicates on broker reconnect)

### MQTT Broker Configuration

- Host: `localhost` (Mosquitto)
- Port: `1884` (WebSockets, non-SSL for local development)
- Remote client connects via: `Paho.Client(MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID)`
- Auto-reconnect with 5s backoff on disconnect

### HTTP vs. MQTT

| Operation | Embedded UI | Remote Control |
|---|---|---|
| Control commands | `POST /api/calibrate` | `MQTT esp32/cmd/calibrate` |
| State updates | Poll `GET /api/status` | Subscribe `esp32/data/state` |
| Live spectra | Poll `GET /api/spectra` | Subscribe `esp32/data/spectra` |
| Live monitor | Poll `GET /api/monitor` | Subscribe `esp32/data/monitor` |
| History | Not available | `GET /history/experiments`, `/history/spectra`, etc. |

## MySQL Database Schema

Managed by `mqtt_to_db.py`. Tables:

```sql
CREATE TABLE experimentos (
  id INT AUTO_INCREMENT PRIMARY KEY,
  exp_id VARCHAR(64) UNIQUE NOT NULL,
  timestamp_ms BIGINT NOT NULL,
  num_measurements INT,
  gain INT,
  mode INT,
  int_cycles INT,
  led_white_ma INT,
  led_ir_ma INT,
  led_uv_ma INT,
  cal_valid BOOLEAN,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE calibraciones (
  exp_id VARCHAR(64) PRIMARY KEY,
  ch1 FLOAT, ch2 FLOAT, ..., ch18 FLOAT,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE mediciones (
  id INT AUTO_INCREMENT PRIMARY KEY,
  exp_id VARCHAR(64),
  meas_index INT,
  ch1 FLOAT, ch2 FLOAT, ..., ch18 FLOAT,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (exp_id) REFERENCES experimentos(exp_id)
);
```

AS7265X 18 channels: 410, 435, 460, 485, 510, 535, 560, 585, 610, 645, 680, 705, 730, 760, 810, 860, 900, 940 nm.

## Flask Backend (`app.py`)

Serves `control.html` and historical data API:

| Route | Method | Purpose |
|---|---|---|
| `/` | GET | Serve `control.html` |
| `/history/experiments` | GET | List experiments (limit, offset, pagination) |
| `/history/spectra` | GET | Get all spectra for exp_id + offsets + wavelengths |
| `/history/export/csv` | GET | Download spectra as CSV (one row per meas) |
| `/history/export/json` | GET | Download experiment as JSON |

Query parameters:
- `/history/experiments?limit=50&offset=0`
- `/history/spectra?exp_id=EXP_001`
- `/history/export/csv?exp_id=EXP_001` or `?all=true`
- `/history/export/json?exp_id=EXP_001` or `?all=true`

CORS: `Access-Control-Allow-Origin: *`

Database: `mysql-connector-python` with `SELECT ... FROM experimentos` / `mediciones` / `calibraciones` JOIN patterns. Parameterized SQL only — no string concatenation.

## Deployment

### Embedded UI (on ESP32)

- Served from `http://192.168.4.1` in AP mode (built into firmware)
- No external dependencies
- Connect to WiFi SSID `Espectrografo-AP`, password `esp32spectro`
- Polls local `/api/*` endpoints directly

### Remote Control System (University Server)

1. **MySQL Database** (one-time setup):
   ```bash
   mysql -u root
   CREATE DATABASE espectrografo;
   USE espectrografo;
   
   CREATE TABLE experimentos (
     id INT AUTO_INCREMENT PRIMARY KEY,
     exp_id VARCHAR(64) UNIQUE NOT NULL,
     timestamp_ms BIGINT NOT NULL,
     num_measurements INT,
     gain INT, mode INT, int_cycles INT,
     led_white_ma INT, led_ir_ma INT, led_uv_ma INT,
     cal_valid BOOLEAN,
     created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
   );
   
   CREATE TABLE calibraciones (
     exp_id VARCHAR(64) PRIMARY KEY,
     ch1 FLOAT, ch2 FLOAT, ch3 FLOAT, ch4 FLOAT, ch5 FLOAT, ch6 FLOAT,
     ch7 FLOAT, ch8 FLOAT, ch9 FLOAT, ch10 FLOAT, ch11 FLOAT, ch12 FLOAT,
     ch13 FLOAT, ch14 FLOAT, ch15 FLOAT, ch16 FLOAT, ch17 FLOAT, ch18 FLOAT,
     created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
   );
   
   CREATE TABLE mediciones (
     id INT AUTO_INCREMENT PRIMARY KEY,
     exp_id VARCHAR(64),
     meas_index INT,
     ch1 FLOAT, ch2 FLOAT, ch3 FLOAT, ch4 FLOAT, ch5 FLOAT, ch6 FLOAT,
     ch7 FLOAT, ch8 FLOAT, ch9 FLOAT, ch10 FLOAT, ch11 FLOAT, ch12 FLOAT,
     ch13 FLOAT, ch14 FLOAT, ch15 FLOAT, ch16 FLOAT, ch17 FLOAT, ch18 FLOAT,
     created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
     FOREIGN KEY (exp_id) REFERENCES experimentos(exp_id)
   );
   ```

2. **MQTT Broker** (Mosquitto):
   ```bash
   sudo apt-get install mosquitto mosquitto-clients
   sudo systemctl start mosquitto
   ```
   Edit `/etc/mosquitto/mosquitto.conf` to enable WebSocket listener on port 1884:
   ```
   listener 1884
   protocol websocket
   ```

3. **Flask App** (on university server):
   ```bash
   pip install -r requirements.txt
   python app.py
   ```
   Serves `control.html` at `http://localhost:5000` and `/history/*` API routes.

4. **MQTT Bridge** (in background):
   ```bash
   python mqtt_to_db.py &
   ```
   Subscribes to `esp32/data/*` and stores in MySQL.

5. **ESP32 WiFi STA Mode** (to reach the MQTT broker):
   - Use embedded UI "WiFi / Send to DB" button to connect to lab network
   - Once STA is connected, firmware publishes to `esp32/data/*` topics
   - MQTT broker must be accessible from ESP's network

### Troubleshooting

- **control.html won't connect**: Verify MQTT broker is running (`mosquitto` on localhost:1884 with WebSocket)
- **No history data**: Check `mqtt_to_db.py` is running and MySQL connection works
- **CSV/JSON export fails**: Verify `control.html` is in the same directory as `app.py`
- **ESP32 not publishing**: Ensure it's in STA mode (connected to lab WiFi), not AP mode

## Verify-Before-Delete (SD ↔ DB consistency)

The SD card keeps raw CSV as the source of truth until MySQL is confirmed to hold the same data. Sequence:

1. `SDLogger::saveExperiment()` appends rows to `/spectra.csv` AND writes `/pending/<exp_id>.json` = `{exp_id, expected_rows, saved_at_ms}`
2. In parallel, the ESP32 publishes `esp32/data/upload` via MQTT; `mqtt_to_db.py` inserts rows into `mediciones`
3. Periodically (recommended: every 30 s while STA WiFi is up), the ESP32 calls `g_sdLogger.cleanupVerifiedExperiments(MQTT_BROKER_HOST, 5000)`
4. For each pending flag, ESP32 issues `GET /verify?exp_id=...&expected=N` against the Flask server
5. Flask queries `SELECT COUNT(*) FROM mediciones WHERE exp_id=?` and returns `{verified, rows_found, rows_expected, experiment_registered}`
6. On `verified:true`, ESP32 calls `removeExperimentRows()` (atomic CSV rewrite via `/spectra.tmp`) and then `clearPending()`

**Crash safety invariants:**
- A flag file is created BEFORE deletion can be considered. If the ESP reboots between save and verify, the flag survives → cleanup resumes on next boot
- The CSV rewrite uses a tmp file + rename. If power is lost mid-rewrite, either the original or the new file is intact — never both corrupt
- A `verified:false` or unreachable server is treated identically: flag stays, retry next pass. **Rows are never deleted without an explicit `verified:true` from the server**

**Retry policy** (`sd_logger.cpp`):
- `VERIFY_MAX_RETRIES = 3` (per cleanup pass per experiment)
- `VERIFY_TIMEOUT_MS = 5000` (per HTTP GET)
- Inter-attempt sleep: 500 ms

**Integration point** (`main.cpp`, not yet wired):
```cpp
static unsigned long _lastCleanup = 0;
void loop() {
    // ... existing state machine tick ...
    if (millis() - _lastCleanup > 30000 && WiFi.status() == WL_CONNECTED) {
        _lastCleanup = millis();
        g_sdLogger.cleanupVerifiedExperiments(MQTT_BROKER_HOST, 5000);
    }
}
```

## Known Bug Fixes

### Chart blank-render (fixed)

**Symptom:** "Spectra (last 3 measurements)" intermittently blank; worsens near measCount=20; never renders again after POST `/api/config` until reboot.

**Root cause:** On high-irradiance reads or after `applyConfig()` put the sensor in a transient bad state, `takeMeasurement()` could return `NaN`/`Inf` floats. `api_routes.cpp` serialized these as `"nan"` — invalid JSON — so `fetch().json()` rejected and `drawChart()` was never called. Cascading miss: once any bad value landed in `spectra[20][18]`, every subsequent `/api/spectra` poll would include it, poisoning every future render.

**Fix:** `api_routes.cpp::handleGetSpectra` (and `/api/calibration`, `/api/monitor`) now pass floats through `isfinite()` and substitute `0.0` on non-finite. `html_content.h::drawChart` additionally sanitizes on the client, uses `ctx.setTransform(1,0,0,1,0,0)` before re-scaling to avoid compounded DPR scaling, skips non-finite points in path drawing, and has a `clearChart()` helper called on `count==0` or after `applyCfg()` so the stale last frame does not linger after a config change.

### N>20 clamp warning (fixed)

**Symptom:** Requesting `N=25` silently clamped to 20 with no user feedback.

**Fix:** `handleSetConfig` now returns `{status:"config_applied", N:<effective>, warning:"..."}` when clamping occurs. The embedded UI shows a yellow dismissible banner via `showWarning()` and auto-corrects the `measN` input. Warning is also logged to the activity log.

## ESP32 MQTT Implementation (TODO for firmware)

The remote control system requires MQTT publishing support in the firmware. Skeleton:

1. **Add dependency**: PubSubClient or Arduino-MQTT in `platformio.ini`

2. **MQTT client singleton** in `src/core/mqtt_client.h`:
   - Global `g_mqttClient` instance
   - Connect on STA WiFi ready
   - Disconnect on STA loss

3. **State transition hook** (in `state_machine.cpp` `exitState()`):
   ```cpp
   publishToMqtt("esp32/data/state", {
     "state": newState,
     "timestamp_ms": millis(),
     "measCount": g_measurementEngine.getCount(),
     "measTarget": g_config.N,
     ...
   });
   ```

4. **Heartbeat timer** (~5s in main loop):
   ```cpp
   publishToMqtt("esp32/data/status", {
     "state": g_stateMachine.getState(),
     "sensorReady": g_sensorDriver.isReady(),
     "sdReady": g_sdLogger.isReady(),
     "calValid": g_calibration.isValid(),
     "rssi": WiFi.RSSI(),
     "heap": ESP.getFreeHeap()
   });
   ```

5. **Spectra publish** (after `g_measurementEngine` completes):
   ```cpp
   publishToMqtt("esp32/data/spectra", {
     "exp_id": g_config.expId,
     "spectra": [18×N floats],
     "offsets": g_calibration.offset[18],
     "wavelengths": [410, 435, ..., 940],
     "count": N,
     "timestamp_ms": g_experiment.timestamp
   });
   ```

6. **Upload publish** (after `g_sdLogger.saveExperiment()`):
   ```cpp
   publishToMqtt("esp32/data/upload", {
     "exp_id": g_config.expId,
     "timestamp_ms": millis()
   });
   ```

7. **Live monitor publish** (in live monitor loop):
   ```cpp
   if (g_liveReady) {
     publishToMqtt("esp32/data/monitor", {
       "ready": true,
       "ch": g_liveBuf[18],
       "wl": [410, 435, ..., 940]
     });
   }
   ```

8. **Command subscription** (on MQTT connect):
   ```cpp
   client.subscribe("esp32/cmd/#");
   // onMessage: extract topic, check state guard, call requestTransition()
   ```

Until MQTT is implemented, the embedded UI and remote control via HTTP will still work independently.
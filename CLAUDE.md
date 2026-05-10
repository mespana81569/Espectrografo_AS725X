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

No automated tests — this is embedded firmware. Verification is via serial monitor, the local web UI (192.168.4.1), and the remote dashboard (`server/control.html`).

## Project Summary

ESP32 portable water-analysis spectrograph using the AS7265X 18-channel spectral sensor (410–940 nm). The system runs a state machine in `loop()`, with a non-blocking HTTP web server (ESPAsyncWebServer) for the local HMI and an MQTT client (PubSubClient) that mirrors the same control surface to a remote dashboard. Data is saved to a microSD card in CSV (v3 schema), then bulk-uploaded over MQTT, then verified against a MySQL DB before being purged from SD.

## Architecture Overview

### State Machine Flow

```
IDLE → CALIBRATION → WAIT_CONFIRMATION → MEASUREMENT → VALIDATION → SAVE_DECISION → IDLE
IDLE → LIVE_MONITOR → IDLE
```

- `g_stateMachine.tick()` called every 10 ms from `loop()`
- State transitions via `requestTransition()` — applied on next tick
- Web API handlers run on Core 0 (AsyncTCP ISR context); MQTT callbacks run synchronously in `_client.loop()`. **Both** only set deferred-command flags — never block, never touch SD/sensor.
- Calibration and measurement engines are polled in `loop()` only when in the matching state
- `MqttClient::tick()` runs every loop pass on Core 1 — handles reconnect, pumps protocol, processes deferred commands, drives the SD bulk-upload state machine, publishes heartbeat + live frames
- A periodic cleanup pass (every 30 s, STA-connected, idle states only) HTTP-GETs `/verify` for each pending experiment and rewrites `/spectra.csv` to drop verified rows

#### Detailed Workflow

```
[IDLE]
  ↓ user clicks "1. Start Calibration" (local UI POST /api/calibrate
    OR remote dashboard publish esp32/cmd/calibrate)
  → requestTransition(CALIBRATION)

[CALIBRATION]
  g_calibration.tick() collects N samples × 500 ms (N = nCal, or = num_measurements
  when SensorConfig::nCalUseSameAsN is true).
  On completion, calibration snapshots the active SensorConfig into
  CalibrationData::cfg_at_cal — used later to invalidate UI plots when the
  user changes gain/integration/LEDs without recalibrating.
  StateMachine::tick() auto-transitions → exitState(CALIBRATION)
    → g_calibration.clearDoneFlag()
  → enterState(WAIT_CONFIRMATION)
  (calibration is NOT saved to SD here — it's embedded per row in
   /spectra.csv at saveExperiment time, alongside the measurement data.)

[WAIT_CONFIRMATION]
  User physically inserts sample cuvette
  POST /api/confirm OR esp32/cmd/confirm → requestTransition(MEASUREMENT)

[MEASUREMENT]
  g_measurementEngine.tick() collects N spectra at 500 ms intervals.
  At completion, computeProcessed() fills exp.transmittance and
  exp.absorbance using the snapshotted calibration reference.
  StateMachine::tick() auto-transitions → VALIDATION

[VALIDATION]
  POST /api/accept OR esp32/cmd/accept → requestTransition(SAVE_DECISION)

[SAVE_DECISION]
  POST /api/save OR esp32/cmd/save:
    → g_sdLogger.saveExperiment(exp)  // appends rows + writes /pending/<uuid>.json
    → g_mqttClient.publishExperiment(exp)  // immediate single-experiment push
    → IDLE
  POST /api/discard OR esp32/cmd/discard → IDLE

[LIVE_MONITOR]
  Entered from IDLE via POST /api/monitor/start OR esp32/cmd/monitor/start
  loop() reads sensor continuously into g_liveBuf[18]
  GET /api/monitor returns live channel data
  MqttClient publishes monitor frames at LIVE_INTERVAL_MS (500 ms) to
  esp32/data/monitor for the dashboard
  POST /api/monitor/stop OR esp32/cmd/monitor/stop → IDLE
```

### Module Responsibilities

| Module | File | Responsibility |
|---|---|---|
| State Machine | `src/core/state_machine.cpp` | `SystemState` enum, transitions, `enterState`/`exitState` hooks; publishes state name to MQTT on entry |
| Sensor Driver | `src/sensors/as7265x_driver.cpp` | Wraps SparkFun AS7265X lib; applies `SensorConfig`; reads 18 channels |
| Calibration | `src/acquisition/calibration.cpp` | N-sample blank reference average (N from SensorConfig); snapshots `cfg_at_cal`; produces `CalibrationData` with offset/reference[18] |
| Measurement Engine | `src/acquisition/measurement_engine.cpp` | N sequential readings at 500 ms intervals; stores raw Δ + computes T% + A; assigns RFC 4122 v4 `uuid` per experiment |
| SD Logger | `src/storage/sd_logger.cpp` | VSPI (MOSI=23, MISO=19, SCK=18, CS=5); v3 CSV (86 cols) **FILE_APPEND** to `/spectra.csv`; pending flags in `/pending/<uuid>.json`; verify-and-purge against `/verify` HTTP endpoint |
| Web Server | `src/web/web_server.cpp` | WiFi AP + scan + STA connection; HTTP server lifecycle |
| API Routes | `src/web/api_routes.cpp` | REST endpoints; all responses include `Cache-Control: no-store` |
| MQTT Client | `src/mqtt/mqtt_client.cpp` | PubSubClient wrapper; deferred command dispatch; heartbeat, live frame publishers; SD bulk upload state machine |
| Embedded Frontend | `src/ui/html_content.h` | Single-page HTML/CSS/JS in PROGMEM; WiFi panel; live chart with transmittance/absorbance views |

### REST API Endpoints (local 192.168.4.1)

| Endpoint | Method | State Guard | Purpose |
|---|---|---|---|
| `/api/status` | GET | — | State, sensorReady, sdReady, calValid, calN, cal/meas progress, uuid, expId, calCfg vs liveCfg |
| `/api/config` | GET | — | Current sensor configuration (incl. `nCal`, `nCalUseSameAsN`) |
| `/api/config` | POST | IDLE only | Set gain, integration, LEDs, N, expId, nCal config |
| `/api/calibrate` | POST | IDLE only | Begin blank reference calibration |
| `/api/confirm` | POST | WAIT_CONFIRMATION | Sample inserted, proceed to measurement |
| `/api/measure` | POST | IDLE or WAIT_CONFIRMATION | Start measurement directly |
| `/api/spectra` | GET | — | Raw Δ spectra + wavelengths |
| `/api/transmittance` | GET | — | Transmittance % per channel per measurement |
| `/api/absorbance` | GET | — | Absorbance per channel per measurement |
| `/api/calibration` | GET | — | Current calibration offsets, reference, cfg_at_cal, n_used |
| `/api/accept` | POST | VALIDATION | Proceed to save dialog |
| `/api/save` | POST | SAVE_DECISION | Write experiment to SD + MQTT publish, return to IDLE |
| `/api/discard` | POST | SAVE_DECISION | Discard data, return to IDLE |
| `/api/monitor/start` | POST | IDLE | Enter live monitor mode |
| `/api/monitor/stop` | POST | LIVE_MONITOR | Exit live monitor |
| `/api/monitor` | GET | — | Live 18-channel reading (`g_liveBuf`) |
| `/api/wifi` | GET | — | WiFi STA connection status string |
| `/api/wifi` | POST | — | Connect: `{"ssid":"...","password":"..."}` |
| `/api/wifi/scan` | POST | — | Trigger network scan (non-blocking, driven from loop) |
| `/api/wifi/scan` | GET | — | Scan status + cached results |

## MQTT Architecture

### Broker

Broker host + credentials live in `firmware/secrets.h` (gitignored — copy from
[firmware/secretsExample.h](firmware/secretsExample.h)) and are pulled into
[firmware/src/mqtt/mqtt_client.h](firmware/src/mqtt/mqtt_client.h) via
`#include "../secrets.h"`:

```
HOST              "<public IP or domain>"  // shared with /verify (Flask)
MQTT_USERNAME     "espectrografo"           // matches docker/mosquitto/passwd
MQTT_PASSWORD     "<from docker/.env>"
FLASK_API_KEY     "<matches docker/.env API_KEY>"

// In mqtt_client.h:
MQTT_BROKER_PORT       1883
MQTT_CLIENT_ID         "espectrografo-01"
MQTT_MAX_PACKET_SIZE   16384  // raised from 4096 — see header comment
```

`MQTT_MAX_PACKET_SIZE` was raised because a 20-measurement experiment JSON ≈ 6 KB; the previous 4 KB limit caused `publish()` to silently return false.

`MqttClient::attemptConnect()` calls `_client.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)` so the broker can enforce `allow_anonymous false` (see [docker/mosquitto.conf](docker/mosquitto.conf)).

### Topics

| Topic | Direction | Purpose |
|---|---|---|
| `esp32/cmd/calibrate` | sub | Start calibration |
| `esp32/cmd/confirm` | sub | Confirm sample inserted |
| `esp32/cmd/accept` | sub | Accept validation |
| `esp32/cmd/save` | sub | Save current experiment |
| `esp32/cmd/discard` | sub | Discard current experiment |
| `esp32/cmd/config` | sub | Apply SensorConfig (JSON, ≤768 B buffered) |
| `esp32/cmd/pull_data` | sub | Trigger SD → broker bulk replay of `/spectra.csv` |
| `esp32/cmd/monitor/start` | sub | Enter LIVE_MONITOR |
| `esp32/cmd/monitor/stop` | sub | Exit LIVE_MONITOR |
| `esp32/data/state` | pub | SystemState name on transition |
| `esp32/data/spectra` | pub | Single experiment JSON after SAVE_DECISION |
| `esp32/data/upload` | pub | Bulk-upload experiment JSON (one per group during pull) |
| `esp32/data/upload/error` | pub | Per-uuid error after `UPLOAD_MAX_RETRIES` (=3) |
| `esp32/data/status` | pub | 5 s heartbeat (state, RSSI) |
| `esp32/data/cal_progress` | pub | Live calibration sample count (≤500 ms cadence) |
| `esp32/data/meas_progress` | pub | Live measurement count |
| `esp32/data/monitor` | pub | Live 18-channel frame in LIVE_MONITOR |

### Deferred Command Pattern

PubSubClient invokes its callback synchronously from `_client.loop()`. We never run sensor/SD work from there — the callback only:

1. Sets a `volatile bool _pending*` flag, OR
2. Copies the payload into a fixed `_pendingConfigBuf[768]`

`processPendingCommands()` (called from `tick()` on Core 1) does the real work. This is the same isolation pattern as the AsyncWebServer handlers.

### SD Bulk Upload (`pull_data`)

Triggered by `esp32/cmd/pull_data`. State machine in `MqttClient`:

```
IDLE → OPENING → READING → FINISHING → IDLE
```

- Reads `/spectra.csv` line by line, groups consecutive rows by **`uuid`** (column 0, primary key — see R1 in design notes)
- A row with a different uuid is stashed in `s_lookaheadLine` and processed as the first row of the next group on the following tick
- `UploadGroup` carries the full 86-column row data: metadata, calibration offsets, raw Δ, transmittance, absorbance — **no recomputation** in the firmware bulk path. The values were computed at acquisition time by `computeProcessed()` and persisted; the bulk path only re-emits.
- Per-group publish retry: failed `_client.publish()` keeps the buffered group and retries up to `UPLOAD_MAX_RETRIES = 3` before publishing an error event and skipping. One bad payload no longer wedges the whole pull.

### Heartbeat & Live Frames

`tick()` publishes at `LIVE_INTERVAL_MS = 500 ms` (cal_progress, meas_progress, monitor) and `HEARTBEAT_INTERVAL_MS = 5000 ms` (status). Throttling is required so a burst of state changes can't saturate the broker.

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
- Scan results are copied into `ScanNet s_scanResults[20]` (fixed C structs, NOT String/heap) **before** any mode change
- After scan: `WiFi.disconnect(true)` → `WIFI_AP` → `softAP()` → 1500ms settle → `g_httpServer.begin()` → `s_hasResults = true`

### STA Connection

Triggered by POST `/api/wifi`. Runs inline in `webServerLoop()`:

1. `g_httpServer.end()` + AP torn down → `WIFI_STA` + 1000ms settle → `WiFi.begin()`
2. Polls `WiFi.status()` every loop; waits up to **15 seconds** (never bails early on `WL_CONNECT_FAILED` — the driver transiently passes through that state)
3. On success: `g_httpServer.begin()` on STA IP + NTP sync (`configTime()`)
4. On timeout: `restoreAP()` — `WIFI_AP` + `softAP()` + 500ms delay + `g_httpServer.begin()`

## Key Data Types

```cpp
struct SensorConfig {
    SensorGain      gain              = GAIN_16X;
    uint8_t         integrationCycles = 50;
    MeasurementMode mode              = MODE_3;
    uint8_t         ledWhiteCurrent   = 12;   // mA: 12,25,50,100
    uint8_t         ledIrCurrent      = 12;
    uint8_t         ledUvCurrent      = 12;
    bool            ledWhiteEnabled   = false;
    bool            ledIrEnabled      = false;
    bool            ledUvEnabled      = false;
    uint8_t         nCal              = 5;     // blank reference samples
    bool            nCalUseSameAsN    = true;  // when true, nCal = num_measurements
};

// Use sensorConfigCountsComparable(a, b) before plotting transmittance —
// counts scale with gain × integration time × per-LED state, so any change
// invalidates a previous I0 reference.

struct CalibrationData {
    bool         valid;
    float        offset[18];          // (raw - reference) baseline used as Δ0
    float        reference[18];       // raw blank average — divisor for T%
    SensorConfig cfg_at_cal;          // snapshot at calibration end
    uint8_t      n_used;              // samples actually averaged
};

struct Experiment {
    char            experiment_id[64];
    char            uuid[37];                          // RFC 4122 v4 — primary key
    uint32_t        timestamp;                         // millis() at start
    int             num_measurements;                  // target N (1–20)
    SensorConfig    sensor_cfg;
    CalibrationData calibration;
    float           spectra      [20][18];             // raw Δ counts
    float           transmittance[20][18];             // %, 0..100
    float           absorbance   [20][18];             // a.u.
    int             count;
    bool            processed;                         // T+A computed
};
```

`newUuidV4(char out37[37])` uses `esp_random()` (HW RNG) to generate the per-experiment uuid.

## CSV Format — v3 schema (`/spectra.csv`, 86 columns)

One row per measurement; experiments grouped by `uuid` (column 0). Calibration travels **inline per row** so the file is self-contained and parseable in one pass — there is **no companion file**.

```
uuid,exp_id,date,meas_idx,gain,int_cycles,
  white_led,white_mA,ir_led,ir_mA,uv_led,uv_mA,n_cal,cal_valid,
  cal_ch1..cal_ch18,         (18 — blank reference I0 for THIS experiment)
  ch1..ch18,                  (18 — raw Δ counts: sample minus blank)
  t_ch1..t_ch18,              (18 — transmittance %, 0..100)
  a_ch1..a_ch18               (18 — absorbance, a.u.)
```

= 14 metadata + 18 cal + 18 raw + 18 T + 18 A = **86 columns**.

- `date`: ISO `2024-05-01 12:34:56` when NTP sync has occurred; `boot+67s` otherwise
- `gain`: human-readable `"1x"/"4x"/"16x"/"64x"`
- `*_led`: `"ON"/"OFF"`
- `cal_valid`: `1`/`0`
- NaN/Inf are emitted as **empty cells** (between commas) so pandas/spreadsheets read them as NULL.

### Boot-time legacy schema sweep (`SDLogger::begin()`)

Two breaking schema changes have shipped:
- **v1 → v2**: added `uuid` column at index 0
- **v2 → v3**: collapsed `/calibrations.csv` into `/spectra.csv`; added `t_ch*` + `a_ch*`

On boot, `/spectra.csv` is inspected; if its header lacks `t_ch1` it is renamed to `/spectra.legacy.csv` (never deleted). `/calibrations.csv` (v2 companion) is renamed to `/calibrations.legacy.csv`. Pre-uuid pending flags in `/pending/` are removed (they would otherwise block the cleanup pass forever).

## Pending → Verify → Purge Flow

The "save" path is *not* delete-after-publish. SD removal is gated on **server-side confirmation that rows landed in MySQL.**

1. `saveExperiment()` appends rows to `/spectra.csv` and writes `/pending/<uuid>.json` with `{uuid, exp_id, expected_rows, saved_at_ms}` — keyed by uuid (R1) so a rename of `exp_id` between save and verify resolves to the right flag.
2. User-triggered `esp32/cmd/pull_data` bulk-publishes every CSV row to `esp32/data/upload`.
3. `mqtt_to_db.py` (server) inserts into MySQL.
4. `cleanupVerifiedExperiments(host, port)` runs every 30 s in `loop()` (STA-up, idle states only). For each pending flag it HTTP-GETs `http://host:port/verify?uuid=…&expected=N`. On `verified:true`:
   - `removeExperimentRows(uuid)` rewrites `/spectra.csv` → `/spectra.csv.tmp` excluding that uuid, then renames atomically.
   - `clearPending(uuid)` deletes the flag.
5. After the last pending flag clears, the loop also wipes `/spectra.csv` so the SD has no residual data.

`HOST` and `DB_VERIFY_PORT` (5000) come from `firmware/secrets.h` and are read in [firmware/src/main.cpp](firmware/src/main.cpp) — `HOST` is shared with the MQTT broker, so the Flask `/verify` endpoint and the broker must run on the same docker host.

The `/verify` request appends `&token=<FLASK_API_KEY>` so an unauthenticated client can't poll the experiment cardinality. The token is the same `API_KEY` env var the Flask container reads — keep them aligned.

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

```
Index  1    2    3    4    5    6    7    8    9   10   11   12   13   14   15   16   17   18
   nm 410  435  460  485  510  535  560  585  610  645  680  705  730  760  810  860  900  940
```

## Key Timing Constants

| Constant | Value | Location |
|---|---|---|
| `READ_INTERVAL_MS` (calibration & measurement) | 500 ms | `*.h` |
| Sensor blocking read (Mode 3, 50 cycles) | ~420 ms | driver |
| State machine tick | 10 ms | `main.cpp` |
| WiFi scan radio settle (RADIO_OFF) | 500 ms | `web_server.cpp` |
| WiFi scan STA init settle | 1000 ms | `web_server.cpp` |
| WiFi scan AP restore settle | 1500 ms | `web_server.cpp` |
| STA connection settle | 1000 ms | `web_server.cpp` |
| STA connection timeout | 15000 ms | `web_server.cpp` |
| `CALIBRATION_AVERAGES` (fallback) | 5 | `calibration.h` |
| `MAX_MEASUREMENTS` | 20 | `measurement_engine.h` |
| MQTT reconnect backoff | 5000 ms | `mqtt_client.h` |
| MQTT heartbeat | 5000 ms | `mqtt_client.h` |
| MQTT live frame cadence | 500 ms | `mqtt_client.h` |
| MQTT upload max retries / group | 3 | `mqtt_client.h` |
| `MQTT_MAX_PACKET_SIZE` | 16384 | `mqtt_client.h` |
| SD → DB cleanup pass | 30000 ms | `main.cpp` |
| `VERIFY_TIMEOUT_MS` / `VERIFY_MAX_RETRIES` | 5000 / 3 | `sd_logger.h` |
| Serial baud | 115200 | `platformio.ini` |

## Dependencies

Board: `esp32doit-devkit-v1`, framework: `arduino`, partition: `min_spiffs.csv`

| Library | Version | Use |
|---|---|---|
| `sparkfun/SparkFun Spectral Triad AS7265X` | ^1.0.5 | Sensor I2C driver |
| `esphome/ESPAsyncWebServer-esphome` | ^3.2.2 | Non-blocking HTTP server |
| `esphome/AsyncTCP-esphome` | ^2.1.4 | TCP foundation |
| `bblanchon/ArduinoJson` | ^7.2.1 | JSON in API routes + bulk upload |
| `arduino-libraries/SD` | — | MicroSD via SPI |
| `knolleary/PubSubClient` | ^2.8.0 | MQTT client |

Build flags: `-DCORE_DEBUG_LEVEL=0 -DBOARD_HAS_PSRAM`

## Known Bugs Fixed

### SD data loss (`FILE_WRITE` truncation)
`SD.open(LOG_FILE, FILE_WRITE)` on ESP32 maps to `"w"` (truncate). Fixed to `FILE_APPEND`.

### Config zeroed in CSV
`resetExperiment()` `memset(0)` after `configure(cfg)` wiped the config. Fixed by preserving `sensor_cfg` across the memset.

### Date shows raw milliseconds
`exp.timestamp = millis()` is raw ms since boot. Fixed with NTP + `time()` + `strftime()`, fallback to `boot+Xs`.

### Gain/mode saved as raw enum integers
Fixed with `gainStr()` helper returning `"1x"/"4x"/"16x"/"64x"`.

### WiFi scan finds 0 networks in AP mode
Single 2.4 GHz radio cannot scan while serving AP clients. Fixed by full radio cycle: AP off → STA → scan → AP on.

### Scan results lost after mode switch
`WiFi.SSID(i)` invalidated when driver mode changes. Fixed by copying into `ScanNet s_scanResults[20]` (fixed C structs) before any mode change.

### STA connection fails too quickly
`WL_CONNECT_FAILED` fires transiently during DHCP. Fixed by removing early-exit on that status — only the 15 s timeout terminates.

### `netstack cb reg failed with 12308`
Caused by `WIFI_OFF → WIFI_STA → WIFI_OFF → WIFI_AP` (double netif init). Fixed by going `STA → AP` directly after scan.

### MQTT `publish()` silently dropped large experiments
`MQTT_MAX_PACKET_SIZE` default of 4 KB was below the ~6 KB JSON of a 20-measurement experiment. Raised to 16 KB.

### Bulk upload wedged on a single bad publish
`_client.publish()` failure used to log "FAIL" and skip. Fixed with per-group retry up to `UPLOAD_MAX_RETRIES = 3`, then a `esp32/data/upload/error` event.

### exp_id rename broke pending verification
Pending flags were keyed on `exp_id` — a rename between save and verify orphaned the flag. Fixed by switching the primary key to RFC 4122 v4 `uuid` (R1). exp_id remains as a user-facing label only.

### Stale calibration plotted as transmittance
Changing gain/integration after calibration produces incomparable counts. Fixed by snapshotting `cfg_at_cal` into `CalibrationData`; UI uses `sensorConfigCountsComparable()` to show calibration-invalid state.

### v2 schema rows silently skipped on upload
Bulk upload parser expected v3 columns; v1/v2 rows had wrong column count and were silently dropped. Fixed by boot-time schema sweep that renames `/spectra.csv` to `/spectra.legacy.csv` if the header lacks `t_ch1`.

### `nan` literals broke chart JSON
`String(nan, 4)` → `"nan"` made `JSON.parse` fail on the dashboard. Fixed by emitting `null` for NaN/Inf in JSON, and empty cells in CSV.

## Backend Stack (Docker)

A four-service Compose stack under [docker-compose.yml](docker-compose.yml) provides storage, a REST API, a remote web UI, and an MQTT ingestion path.

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

### Config files

- [docker/mosquitto.conf](docker/mosquitto.conf) — two listeners (1883 TCP, 9001 WebSockets), `allow_anonymous false`, reads `password_file /mosquitto/config/passwd`. **Must be UTF-8 without BOM** (Notepad will break this). The `passwd` file is gitignored — generate it with `mosquitto_passwd -c -b ./docker/mosquitto/passwd <user> <password>`.
- [docker/.env](docker/.env) — gitignored. Copy from [docker/.env.example](docker/.env.example) and fill in. Carries DB credentials, MQTT broker auth, the ESP32 `/verify` token, the Flask session secret + dashboard login, and the public MQTT host the browser uses for WebSockets.
- [docker/mysql-init/init.sql](docker/mysql-init/init.sql) — creates the schema (`experimentos`, `mediciones`, `calibraciones`, `transmittances`, `absorbancias`). Only runs when `mysql_data` volume is empty. Does NOT create users — the mysql:8.4 image auto-provisions `MYSQL_USER`/`MYSQL_PASSWORD` from `.env` with privileges scoped to `MYSQL_DATABASE`.

### Auth model

| Surface | Mechanism | Source of truth |
|---|---|---|
| Dashboard pages (`/`, `/history/*`, `/experiments/*`) | Flask session cookie (HMAC-signed via `FLASK_SECRET_KEY`, `SameSite=Lax`, `HttpOnly`) | `LOGIN_USERNAME` / `LOGIN_PASSWORD` in `docker/.env`; compared with `secrets.compare_digest` |
| Dashboard XHR (`apiFetch`) | Same cookie, sent via `credentials: 'same-origin'`; on 401 the wrapper redirects to `/login` | as above |
| Mosquitto (TCP 1883 + WS 9001) | username/password from `mosquitto_passwd` file | `docker/mosquitto/passwd`, generated locally |
| ESP32 `/verify` endpoint | `?token=<FLASK_API_KEY>` query param (cookie-less because the device can't hold a session) | `API_KEY` in `docker/.env`; `FLASK_API_KEY` in `firmware/secrets.h` — must be equal |

`require_login` is dual-mode: returns a 302 to `/login` when `Accept: text/html` is present (browser navigation, file downloads), JSON 401 otherwise (XHR). `/verify` keeps token-only auth so the firmware doesn't need cookie support.

`serve_html()` rewrites `control.html` at request time, injecting `MQTT_PUBLIC_HOST`, `MQTT_PUBLIC_WS_PORT`, `MQTT_USERNAME` and `MQTT_PASSWORD` into the page so the source file stays generic. The Paho JS client picks these up in `connectMQTT()` and authenticates against the broker.

### Server constraints (production: 1 vCPU / 512 MB DigitalOcean droplet)

- `mysql.command:` overrides `--innodb-buffer-pool-size=64M --innodb-log-buffer-size=8M --max-connections=20 --performance-schema=OFF`. Defaults OOM the box at startup.
- The host needs ~1 GB of swap (`fallocate -l 1G /swapfile && mkswap && swapon`) for image pulls and the mysql warm-up.
- `mysql.ports` is replaced with `expose: ["3306"]` — port 3306 is reachable only on the docker bridge network. Flask + mqtt_bridge connect via the service name `mysql`.
- `MYSQL_USER` for both flask and mqtt_bridge is `espectrografo_user` (NOT root). MySQL 8 refuses `root@<any-host>` by default ACL; the dedicated user is auto-created by the mysql image from `MYSQL_USER` / `MYSQL_PASSWORD` in `.env`.

### MySQL schema (`espectrografo` database)

- `experimentos(uuid PK, exp_id, timestamp_ms, num_measurements, gain, mode, int_cycles, led_*_ma, n_cal, cal_valid)` — one row per experiment, **keyed by uuid**
- `mediciones(uuid FK, meas_index, ch1..ch18, t_ch1..t_ch18, a_ch1..a_ch18)` — one row per measurement
- `calibraciones(uuid FK, ch1..ch18)` — one row per experiment (blank reference)

### MQTT → DB ingestion — [server/mqtt_to_db.py](server/mqtt_to_db.py)

Subscribes on `mosquitto:1883`:

| Topic | Handling |
|---|---|
| `esp32/data/upload` | Bulk-replay group → `INSERT IGNORE` into the three tables (uuid PK dedupes) |
| `esp32/data/spectra` | Same handling as `upload` (immediate path after SAVE_DECISION) |
| `esp32/data/status` | Heartbeat logged, not persisted |
| `esp32/data/upload/error` | Logged for visibility |
| `esp32/data/state`, `cal_progress`, `meas_progress`, `monitor` | Forwarded to dashboard via WS, not persisted |

Uses `paho-mqtt` with `CallbackAPIVersion.VERSION2`. Reads `MQTT_BROKER`, `MQTT_PORT`, `MYSQL_*` from env.

### REST API — [server/app.py](server/app.py) (Flask on port 5000)

CORS open (`Access-Control-Allow-Origin: *`).

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | Serves `control.html` |
| `/history/experiments?limit=&offset=` | GET | Paginated experiment list |
| `/history/spectra?uuid=` | GET | Raw Δ spectra + calibration for one experiment |
| `/history/transmittance?uuid=` | GET | Transmittance % per measurement |
| `/history/absorbance?uuid=` | GET | Absorbance per measurement |
| `/history/export/csv?uuid=` or `?all=true` | GET | CSV download |
| `/history/export/json?uuid=` or `?all=true` | GET | JSON download |
| `/experiments/<uuid>` | DELETE | Delete experiment + cascading rows |
| `/experiments/import` | POST | Import a JSON experiment payload (manual upload path) |
| `/verify?uuid=&expected=N` | GET | Confirms `mediciones` row count ≥ N — used by the ESP32 to gate SD purge |

### Remote UI — [server/control.html](server/control.html)

Single-page HTML served by Flask. The `MQTT_HOST` / `MQTT_PORT` literals in the source file are placeholders — `serve_html()` rewrites them at request time from the `MQTT_PUBLIC_HOST` / `MQTT_PUBLIC_WS_PORT` env vars, and prepends `MQTT_USER` / `MQTT_PASS` globals so the Paho client authenticates against the broker. `apiFetch()` wraps every dashboard XHR with `credentials: 'same-origin'` and redirects to `/login` on 401.

### Gotchas

- **BOM in mosquitto.conf** — Windows Notepad writes UTF-8 with BOM; mosquitto fails with `Unknown configuration variable "listener"`. Re-save as UTF-8 (no BOM).
- **Password env mismatch** — code reads `MYSQL_PASSWORD`; MySQL image only honors `MYSQL_ROOT_PASSWORD`. Both must be present and match. Changing `.env` after the data volume exists has no effect — use `docker compose down -v` to re-init.
- **WebSocket listener** — browser MQTT requires `protocol websockets` on a distinct listener from the 1883 TCP one.
- **`MQTT_PORT` default** — `mqtt_to_db.py` defaults to `1883` matching docker-compose; do not change.
- **Broker host coupling** — `HOST` in `firmware/secrets.h` is shared by the MQTT broker AND the Flask `/verify` endpoint, so they must run on the same docker host. Update once.
- **`docker/mosquitto/passwd` is gitignored** — the file must exist on the host before `docker compose up`, otherwise Docker creates an empty directory at that bind-mount path and mosquitto fails to start. Generate with `mosquitto_passwd -c -b ./docker/mosquitto/passwd <user> <password>`.
- **Cookie session vs HTTPS** — `SESSION_COOKIE_SECURE=1` requires HTTPS. Behind plain HTTP the browser will silently drop the cookie and the user appears to log in then immediately get redirected back. Keep it `0` until you put nginx/Caddy in front.
- **`FLASK_SECRET_KEY` empty/missing** — Flask refuses to start (RuntimeError on import). `LOGIN_USERNAME` / `LOGIN_PASSWORD` empty likewise — fail-loud is intentional, a default-empty key would let an attacker forge session cookies.
- **MySQL 8 ACL** — `root@<any-host>` is refused by default. Both Flask and mqtt_bridge must connect with `MYSQL_USER=espectrografo_user`, which the mysql image auto-creates from `.env` on first init. Do not set `MYSQL_USER=root` in compose.
- **512 MB host OOM** — without the `command:` tuning flags the mysql container is killed at startup. Without ~1 GB of swap, `docker compose up` may also OOM during image pull.

## Future Work

- HTTPS + `SESSION_COOKIE_SECURE=1`: front the stack with Caddy or nginx, set `MQTT_PUBLIC_HOST` to the domain, and switch the cookie flag.
- Experiment ID auto-increment is handled in JavaScript after each successful save — no backend counter needed.
- NTP is wired on STA connect; date column auto-corrects once the ESP32 reaches internet.

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
| Web Server | `src/web/web_server.cpp` | WiFi AP mode (SSID: `Espectrografo-AP`, pass: `esp32spectro`, IP: `192.168.4.1`); serves HTML from PROGMEM; optional STA client mode |
| API Routes | `src/web/api_routes.cpp` | REST endpoints (see table below) |
| Frontend | `src/ui/html_content.h` | Full single-page HTML/CSS/JS app embedded as `PROGMEM` string; Chart.js overlay of last 3 spectra; polls `/api/status` every 1.5 s and `/api/spectra` every 3 s |

### REST API Endpoints

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
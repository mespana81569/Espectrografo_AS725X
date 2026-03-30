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
```

- `g_stateMachine.tick()` is called every 10 ms from `loop()`
- State transitions are requested via `requestTransition()` — they are applied on the next tick
- Web API callbacks (ISR context, Core 0) call `requestTransition()` — never block
- Calibration and measurement engines are polled via their own `tick()` inside `loop()` only when the state machine is in the matching state

### Module Responsibilities

| Module | File | Responsibility |
|---|---|---|
| State Machine | `src/core/state_machine.cpp` | Owns `SystemState` enum, drives transitions, calls `enterState`/`exitState` hooks |
| Sensor Driver | `src/sensors/as7265x_driver.cpp` | Wraps SparkFun AS7265X lib; applies `SensorConfig` (gain, int cycles, mode, LED mA); reads all 18 channels |
| Calibration | `src/acquisition/calibration.cpp` | Collects 5-sample blank reference average; produces `CalibrationData` with offset[18]; `applyOffset()` subtracts from spectra |
| Measurement Engine | `src/acquisition/measurement_engine.cpp` | Runs N sequential readings at 300 ms intervals; stores `float spectra[20][18]` in `Experiment` struct in RAM |
| SD Logger | `src/storage/sd_logger.cpp` | VSPI (MOSI=23, MISO=19, SCK=18, CS=5); appends CSV rows to `/spectra_log.csv`; creates header on first write |
| Web Server | `src/web/web_server.cpp` | WiFi AP mode (SSID: `Espectrografo-AP`, pass: `esp32spectro`, IP: `192.168.4.1`); serves HTML from PROGMEM |
| API Routes | `src/web/api_routes.cpp` | REST endpoints: `GET /api/status`, `GET /api/spectra`, `POST /api/config`, `/calibrate`, `/confirm`, `/measure`, `/accept`, `/save`, `/discard` |
| Frontend | `src/ui/html_content.h` | Full single-page HTML/CSS/JS app embedded as `PROGMEM` string; Chart.js overlay of last 3 spectra; polls `/api/status` every 1.5 s and `/api/spectra` every 3 s |

### Key Data Types

- `SensorConfig` — gain, integrationCycles, mode, ledCurrent, ledEnabled
- `CalibrationData` — valid flag, offset[18], reference[18]
- `Experiment` — experiment_id, timestamp, num_measurements, sensor_cfg, calibration, spectra[20][18], count

### Global Singletons

All modules expose a single global instance (`g_stateMachine`, `g_sensorDriver`, `g_calibration`, `g_measurementEngine`, `g_sdLogger`). These are safe to access from web callbacks only for reads or `requestTransition()` — never call blocking sensor/SD operations from a web handler.

### CSV Format

```
date_ms,exp_id,meas_idx,gain,mode,int_cycles,led_ma,ch1,...,ch18
```

One row per individual measurement; multiple rows per experiment sharing the same `exp_id`.

## Hardware Pins

| Function | GPIO |
|---|---|
| I2C SDA (AS7265X) | 21 (default Wire) |
| I2C SCL (AS7265X) | 22 (default Wire) |
| SD MOSI | 23 |
| SD MISO | 19 |
| SD SCK | 18 |
| SD CS | 5 |

## Dependencies

Managed via `platformio.ini`:
- `SparkFun Spectral Triad AS7265X` — sensor driver
- `ESPAsyncWebServer-esphome` + `AsyncTCP-esphome` — non-blocking HTTP
- `ArduinoJson` v7 — JSON serialization in API routes
- `arduino-libraries/SD` — microSD via SPI

## Problems
[IDLE]
  ↓ user clicks "1. Start Calibration"
  POST /api/calibrate → requestTransition(CALIBRATION)  [thread-safe]
  
[CALIBRATION]
  g_calibration.tick() collects 5 samples × 200ms = ~1 second
  g_calibration._done = true
  StateMachine::tick() auto-transitions → exitState(CALIBRATION)
    → g_sdLogger.saveCalibration(...)  ← calibration saved to SD here
    → g_calibration.clearDoneFlag()    ← prevents re-trigger
  → enterState(WAIT_CONFIRMATION)
  
[WAIT_CONFIRMATION]
  UI: btnConfirm enabled, all others disabled
  User physically inserts sample cuvette
  User clicks "2. Insert Sample & Confirm"
  POST /api/confirm → requestTransition(MEASUREMENT)  [thread-safe]
  
[MEASUREMENT]
  g_measurementEngine.tick() collects N spectra
  StateMachine::tick() auto-transitions → VALIDATION
  
[VALIDATION]
  UI: btnAccept enabled
  User clicks "4. Accept & Proceed to Save"
  POST /api/accept → requestTransition(SAVE_DECISION)
  
[SAVE_DECISION]
  UI: btnSave + btnDiscard enabled
  User clicks "5. Save to SD Card"
  POST /api/save → g_sdLogger.saveExperiment(...) → back to IDLE

#include "measurement_engine.h"
#include "../sensors/as7265x_driver.h"
#include "../acquisition/calibration.h"
#include <Arduino.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>

MeasurementEngine g_measurementEngine;

// RFC 4122 v4 UUID — variant 10xx, version 0100 — built from esp_random().
void newUuidV4(char out37[37]) {
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        b[i+0] = (uint8_t)(r >> 24);
        b[i+1] = (uint8_t)(r >> 16);
        b[i+2] = (uint8_t)(r >> 8);
        b[i+3] = (uint8_t)(r >> 0);
    }
    b[6] = (b[6] & 0x0F) | 0x40;   // version 4
    b[8] = (b[8] & 0x3F) | 0x80;   // variant 10
    snprintf(out37, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3],  b[4],b[5],  b[6],b[7],
             b[8],b[9],            b[10],b[11],b[12],b[13],b[14],b[15]);
}

MeasurementEngine::MeasurementEngine()
    : _targetN(1), _running(false), _done(false), _failed(false), _lastReadTime(0) {
    memset(&_experiment, 0, sizeof(_experiment));
}

void MeasurementEngine::configure(int n, const SensorConfig& cfg) {
    _targetN = (n > MAX_MEASUREMENTS) ? MAX_MEASUREMENTS : (n < 1 ? 1 : n);
    _experiment.sensor_cfg = cfg;
    _experiment.num_measurements = _targetN;
}

void MeasurementEngine::start() {
    _running = true;
    _done    = false;
    _failed  = false;
    _experiment.count = 0;
    _experiment.processed = false;
    _experiment.timestamp = millis();
    _experiment.calibration = g_calibration.getData();
    // Snapshot the ACTUAL sensor config at measurement time — this is what
    // gets written to the CSV, so it always matches the hardware state.
    _experiment.sensor_cfg = g_sensorDriver.getConfig();
    // R6 / clarification #5 enforcement at experiment start: if the live
    // sensor config does not match the calibration's snapshotted config, the
    // calibration cannot legitimately be used to compute transmittance.
    // Mark it invalid so all downstream code (UI, MQTT, DB) knows not to
    // produce nonsensical T values.
    if (_experiment.calibration.valid &&
        !sensorConfigCountsComparable(_experiment.calibration.cfg_at_cal,
                                      _experiment.sensor_cfg)) {
        Serial.println("[Acq] Calibration cfg differs from live cfg — invalidating");
        _experiment.calibration.valid = false;
    }
    // Mint a fresh UUID for every acquisition.  Stable across rename and
    // re-save — the user's exp_id label is independent (R1).
    newUuidV4(_experiment.uuid);
    _lastReadTime = 0; // force immediate first read
    Serial.printf("[Acq] Starting %d measurements uuid=%s (gain=%u, int=%u)\n",
                  _targetN, _experiment.uuid,
                  (uint8_t)_experiment.sensor_cfg.gain,
                  _experiment.sensor_cfg.integrationCycles);
}

void MeasurementEngine::tick() {
    if (!_running || _done) return;

    unsigned long now = millis();
    if (now - _lastReadTime < READ_INTERVAL_MS) return;
    _lastReadTime = now;

    int idx = _experiment.count;
    if (!g_sensorDriver.takeMeasurement(_experiment.spectra[idx])) {
        _failed = true;
        _running = false;
        Serial.println("[Acq] Sensor read failed");
        return;
    }

    // Apply calibration offset if available
    g_calibration.applyOffset(_experiment.spectra[idx]);

    _experiment.count++;
    Serial.printf("[Acq] Measurement %d/%d done\n", _experiment.count, _targetN);

    if (_experiment.count >= _targetN) {
        _done    = true;
        _running = false;
        // Compute T+A on-device (verdict: arithmetic cost is negligible
        // vs. the sensor read budget — see "ESP32 feasibility verdict").
        computeProcessed();
        Serial.println("[Acq] Acquisition complete (T+A computed on-device)");
    }
}

// Beer-Lambert reconstruction.  See calibration.cpp for the physics
// docblock; here we just apply it row-by-row.  NaN signals "uncomputable"
// (e.g. I0 ≤ 0 means the calibration channel never registered light, so
// transmittance is undefined for that channel).  The chart code and the
// MQTT JSON serializer both handle NaN explicitly.
void MeasurementEngine::computeProcessed() {
    const CalibrationData& cal = _experiment.calibration;
    for (int m = 0; m < _experiment.count; m++) {
        for (int c = 0; c < NUM_CHANNELS; c++) {
            float I0 = cal.valid ? cal.offset[c] : 0.0f;
            if (!cal.valid || I0 <= 0.0f) {
                _experiment.transmittance[m][c] = NAN;
                _experiment.absorbance[m][c]    = NAN;
                continue;
            }
            float I = _experiment.spectra[m][c] + I0;
            if (I < 0.0f) I = 0.0f;
            float T_pct = (I / I0) * 100.0f;
            _experiment.transmittance[m][c] = T_pct;
            _experiment.absorbance[m][c]    = (T_pct > 0.0f)
                                              ? -log10f(T_pct / 100.0f)
                                              : NAN;
        }
    }
    _experiment.processed = true;
}

bool MeasurementEngine::isDone() const { return _done; }
bool MeasurementEngine::hasFailed() const { return _failed; }

Experiment& MeasurementEngine::getExperiment() { return _experiment; }

void MeasurementEngine::resetExperiment(const char* expId) {
    // Preserve sensor_cfg and num_measurements across resets — only clear
    // spectra data and counters.  The old code did memset(0) which wiped
    // the config that configure() had just stored.
    SensorConfig savedCfg = _experiment.sensor_cfg;
    int savedN = _experiment.num_measurements;

    memset(&_experiment, 0, sizeof(_experiment));
    strncpy(_experiment.experiment_id, expId, sizeof(_experiment.experiment_id) - 1);

    _experiment.sensor_cfg = savedCfg;
    _experiment.num_measurements = savedN;
    _experiment.processed = false;
    _experiment.uuid[0] = 0;            // minted on the next start()
    _running = false;
    _done    = false;
    _failed  = false;
}

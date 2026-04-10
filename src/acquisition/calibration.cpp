#include "calibration.h"
#include "../sensors/as7265x_driver.h"
#include <Arduino.h>
#include <string.h>

Calibration g_calibration;

// Minimum gap between samples.  takeMeasurements() in Mode 3 blocks for
// ~420 ms (50 cycles × 2.8 ms × 3 dies).  Add 500 ms on top so the sensor
// registers have time to settle before the next one-shot trigger.
static const unsigned long CAL_SAMPLE_INTERVAL_MS = 500;

Calibration::Calibration()
    : _running(false), _done(false), _failed(false),
      _samplesCollected(0), _lastSampleTime(0) {
    memset(&_data, 0, sizeof(_data));
    memset(_accumulator, 0, sizeof(_accumulator));
}

void Calibration::start() {
    _running = true;
    _done    = false;
    _failed  = false;
    _samplesCollected = 0;
    _lastSampleTime   = 0;   // force immediate first sample
    memset(_accumulator, 0, sizeof(_accumulator));
    memset(&_data, 0, sizeof(_data));
    Serial.println("[Cal] Calibration started");
}

void Calibration::tick() {
    if (!_running || _done) return;

    unsigned long now = millis();
    if (now - _lastSampleTime < CAL_SAMPLE_INTERVAL_MS) return;
    _lastSampleTime = now;

    float reading[NUM_CHANNELS];
    if (!g_sensorDriver.takeMeasurement(reading)) {
        _failed = true;
        _running = false;
        Serial.println("[Cal] Sensor read failed");
        return;
    }

    for (int i = 0; i < NUM_CHANNELS; i++) {
        _accumulator[i] += reading[i];
    }
    _samplesCollected++;
    Serial.printf("[Cal] Sample %d/%d collected\n", _samplesCollected, CALIBRATION_AVERAGES);

    if (_samplesCollected >= CALIBRATION_AVERAGES) {
        for (int i = 0; i < NUM_CHANNELS; i++) {
            _data.reference[i] = _accumulator[i] / CALIBRATION_AVERAGES;
            _data.offset[i]    = _data.reference[i];
        }
        _data.valid = true;
        _done    = true;
        _running = false;
        Serial.println("[Cal] Calibration complete");
    }
}

bool Calibration::isDone() const { return _done; }
bool Calibration::hasFailed() const { return _failed; }
void Calibration::clearDoneFlag() { _done = false; }

const CalibrationData& Calibration::getData() const { return _data; }

void Calibration::reset() {
    _running = false;
    _done    = false;
    _failed  = false;
    _samplesCollected = 0;
    memset(&_data, 0, sizeof(_data));
    memset(_accumulator, 0, sizeof(_accumulator));
}

void Calibration::applyOffset(float* inOut18) const {
    if (!_data.valid) return;
    for (int i = 0; i < NUM_CHANNELS; i++) {
        inOut18[i] -= _data.offset[i];
        if (inOut18[i] < 0.0f) inOut18[i] = 0.0f;
    }
}

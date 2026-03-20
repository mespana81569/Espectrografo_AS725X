#include "calibration.h"
#include "../sensors/as7265x_driver.h"
#include <Arduino.h>
#include <string.h>

Calibration g_calibration;

Calibration::Calibration()
    : _running(false), _done(false), _failed(false), _samplesCollected(0) {
    memset(&_data, 0, sizeof(_data));
    memset(_accumulator, 0, sizeof(_accumulator));
}

void Calibration::start() {
    _running = true;
    _done    = false;
    _failed  = false;
    _samplesCollected = 0;
    memset(_accumulator, 0, sizeof(_accumulator));
    memset(&_data, 0, sizeof(_data));
    Serial.println("[Cal] Calibration started");
}

void Calibration::tick() {
    if (!_running || _done) return;

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
            _data.offset[i]    = _data.reference[i]; // offset = blank mean
        }
        _data.valid = true;
        _done    = true;
        _running = false;
        Serial.println("[Cal] Calibration complete");
    }

    // Inter-sample delay for stable readings
    delay(200);
}

bool Calibration::isDone() const { return _done; }
bool Calibration::hasFailed() const { return _failed; }

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

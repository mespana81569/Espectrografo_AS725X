#include "measurement_engine.h"
#include "../sensors/as7265x_driver.h"
#include "../acquisition/calibration.h"
#include <Arduino.h>
#include <string.h>

MeasurementEngine g_measurementEngine;

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
    _experiment.timestamp = millis();
    _experiment.calibration = g_calibration.getData();
    _lastReadTime = 0; // force immediate first read
    Serial.printf("[Acq] Starting %d measurements\n", _targetN);
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
        Serial.println("[Acq] Acquisition complete");
    }
}

bool MeasurementEngine::isDone() const { return _done; }
bool MeasurementEngine::hasFailed() const { return _failed; }

Experiment& MeasurementEngine::getExperiment() { return _experiment; }

void MeasurementEngine::resetExperiment(const char* expId) {
    memset(&_experiment, 0, sizeof(_experiment));
    strncpy(_experiment.experiment_id, expId, sizeof(_experiment.experiment_id) - 1);
    _running = false;
    _done    = false;
    _failed  = false;
}

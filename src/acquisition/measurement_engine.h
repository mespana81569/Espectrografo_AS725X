#pragma once
#include "../sensors/as7265x_driver.h"
#include "../acquisition/calibration.h"
#include <stdint.h>

#define MAX_MEASUREMENTS 20
#define NUM_CHANNELS     18

struct Experiment {
    char      experiment_id[32];
    uint32_t  timestamp;          // millis() at start
    int       num_measurements;
    SensorConfig sensor_cfg;
    CalibrationData calibration;
    float     spectra[MAX_MEASUREMENTS][NUM_CHANNELS];
    int       count;              // how many spectra actually stored
};

class MeasurementEngine {
public:
    MeasurementEngine();
    void configure(int n, const SensorConfig& cfg);
    void start();
    void tick();
    bool isDone() const;
    bool hasFailed() const;

    Experiment& getExperiment();
    void resetExperiment(const char* expId);

private:
    Experiment _experiment;
    int  _targetN;
    bool _running;
    bool _done;
    bool _failed;
    unsigned long _lastReadTime;
    static const unsigned long READ_INTERVAL_MS = 300;
};

extern MeasurementEngine g_measurementEngine;

#pragma once
#include "../sensors/as7265x_driver.h"
#include "../acquisition/calibration.h"
#include <stdint.h>

#define MAX_MEASUREMENTS 20
#define NUM_CHANNELS     18

struct Experiment {
    char      experiment_id[64];
    char      uuid[37];           // RFC 4122 v4 string (36 + NUL).  Stable PK
                                  // across rename/save — see R1 in design doc.
    uint32_t  timestamp;          // millis() at start
    int       num_measurements;
    SensorConfig sensor_cfg;
    CalibrationData calibration;
    float     spectra[MAX_MEASUREMENTS][NUM_CHANNELS];        // raw Δ counts
    float     transmittance[MAX_MEASUREMENTS][NUM_CHANNELS];  // %, 0..100
    float     absorbance[MAX_MEASUREMENTS][NUM_CHANNELS];     // a.u.
    int       count;              // how many spectra actually stored
    bool      processed;          // true once T+A have been computed
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

    // Compute transmittance % and absorbance for every stored measurement
    // using the snapshotted calibration.  Idempotent; sets exp.processed.
    void computeProcessed();

private:
    Experiment _experiment;
    int  _targetN;
    bool _running;
    bool _done;
    bool _failed;
    unsigned long _lastReadTime;
    static const unsigned long READ_INTERVAL_MS = 500;
};

// RFC 4122 v4 UUID (36 chars + NUL = 37 bytes).  Uses esp_random() which is
// hardware-RNG-backed on the ESP32 — uniqueness is essentially guaranteed.
void newUuidV4(char out37[37]);

extern MeasurementEngine g_measurementEngine;

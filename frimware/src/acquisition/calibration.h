#pragma once
#include <stdint.h>
#include "../sensors/as7265x_driver.h"

#define NUM_CHANNELS 18
// Default if SensorConfig::nCal is 0 (legacy save with no value).  The actual
// runtime average count comes from the SensorConfig the user posts via
// /api/config — see Calibration::start(cfg) below.
#define CALIBRATION_AVERAGES 5

struct CalibrationData {
    bool         valid;
    float        offset[NUM_CHANNELS];    // blank reference (noise baseline)
    float        reference[NUM_CHANNELS]; // raw blank reading for normalization
    // Snapshot of the sensor configuration at the moment calibration finished.
    // The dashboard compares this against the current SensorConfig and refuses
    // to plot transmittance when they diverge — counts are gain/int-cycle
    // dependent and a stale I0 reference is meaningless across config changes.
    SensorConfig cfg_at_cal;
    uint8_t      n_used;                  // number of samples actually averaged
};

class Calibration {
public:
    Calibration();
    // n_target is the number of one-shot samples to average for the blank.
    // When 0 the start() implementation falls back to CALIBRATION_AVERAGES.
    void start(uint8_t n_target = 0);
    void tick();
    bool isDone() const;
    bool hasFailed() const;
    void clearDoneFlag();
    int  samplesCollected() const;     // for live progress publishing
    int  samplesTarget() const;        // for live progress publishing

    const CalibrationData& getData() const;
    void reset();

    // Apply calibration offset to a raw reading
    void applyOffset(float* inOut18) const;

private:
    CalibrationData _data;
    bool          _running;
    bool          _done;
    bool          _failed;
    int           _samplesCollected;
    int           _samplesTarget;
    float         _accumulator[NUM_CHANNELS];
    unsigned long _lastSampleTime;
};

extern Calibration g_calibration;

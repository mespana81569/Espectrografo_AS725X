#pragma once
#include <stdint.h>

#define NUM_CHANNELS 18
#define CALIBRATION_AVERAGES 5

struct CalibrationData {
    bool    valid;
    float   offset[NUM_CHANNELS];   // blank reference (noise baseline)
    float   reference[NUM_CHANNELS]; // raw blank reading for normalization
};

class Calibration {
public:
    Calibration();
    void start();
    void tick();
    bool isDone() const;
    bool hasFailed() const;
    void clearDoneFlag();

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
    float         _accumulator[NUM_CHANNELS];
    unsigned long _lastSampleTime;
};

extern Calibration g_calibration;

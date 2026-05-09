#pragma once
#include <SparkFun_AS7265X.h>

// Gain values matching AS7265X library constants
enum class SensorGain : uint8_t {
    GAIN_1X  = 0,
    GAIN_4X  = 1,
    GAIN_16X = 2,
    GAIN_64X = 3
};

// Measurement modes
enum class MeasurementMode : uint8_t {
    MODE_0 = 0, // 4 channels out of 6 (VBGYOR)
    MODE_1 = 1, // Different 4 channels (RTUVWX)
    MODE_2 = 2, // All 6 channels via 2 reads
    MODE_3 = 3  // One-shot all 18 channels
};

struct SensorConfig {
    SensorGain      gain              = SensorGain::GAIN_16X;
    uint8_t         integrationCycles = 50;   // 2.8ms per cycle, 50 = ~140ms
    MeasurementMode mode              = MeasurementMode::MODE_3;
    uint8_t         ledWhiteCurrent   = 12;   // mA: 12, 25, 50, 100
    uint8_t         ledIrCurrent      = 12;
    uint8_t         ledUvCurrent      = 12;
    bool            ledWhiteEnabled   = false;
    bool            ledIrEnabled      = false;
    bool            ledUvEnabled      = false;
    // N samples averaged for the blank reference at calibration time.  When
    // `nCalUseSameAsN` is true (Option A from clarification #6) this field is
    // ignored and CALIBRATION_AVERAGES = N (the measurement count).
    uint8_t         nCal              = 5;
    bool            nCalUseSameAsN    = true;
};

// True when two configs would produce comparable counts for transmittance.
// Counts scale with gain × integration time × per-LED state, so any of those
// changing makes a previous I0 reference unusable as a divisor.  Mode change
// is included because non-Mode-3 reads emit fewer channels.
inline bool sensorConfigCountsComparable(const SensorConfig& a, const SensorConfig& b) {
    return a.gain              == b.gain
        && a.integrationCycles == b.integrationCycles
        && a.mode              == b.mode
        && a.ledWhiteEnabled   == b.ledWhiteEnabled
        && a.ledIrEnabled      == b.ledIrEnabled
        && a.ledUvEnabled      == b.ledUvEnabled
        && a.ledWhiteCurrent   == b.ledWhiteCurrent
        && a.ledIrCurrent      == b.ledIrCurrent
        && a.ledUvCurrent      == b.ledUvCurrent;
}

class AS7265xDriver {
public:
    AS7265xDriver();
    bool begin();
    bool isReady() const;

    void applyConfig(const SensorConfig& cfg);
    SensorConfig getConfig() const;

    // Read all 18 channels into provided array (float[18])
    bool takeMeasurement(float* out18);

    void setSleepMode(bool sleep);

    // Channel wavelengths (nm) for all 18 channels in order A-R
    static const uint16_t WAVELENGTHS[18];

private:
    AS7265X _sensor;
    SensorConfig _cfg;
    bool _initialized;
};

extern AS7265xDriver g_sensorDriver;

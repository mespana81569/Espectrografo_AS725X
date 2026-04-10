#pragma once
#include "../acquisition/measurement_engine.h"
#include "../acquisition/calibration.h"

#define SD_CS_PIN   5
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#define SD_SCK_PIN  18
#define LOG_FILE    "/spectra.csv"
#define CAL_FILE    "/calibration.csv"

class SDLogger {
public:
    SDLogger();
    bool begin();
    bool isReady() const;

    // Save full experiment to SD (APPENDS CSV rows — never truncates)
    bool saveExperiment(const Experiment& exp);

    // Save calibration data to SD (overwrites previous)
    bool saveCalibration(const CalibrationData& cal);

    // Write CSV header if file is new or empty
    bool ensureHeader();

private:
    bool _ready;
};

extern SDLogger g_sdLogger;

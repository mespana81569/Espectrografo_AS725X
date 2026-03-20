#include "sd_logger.h"
#include <SD.h>
#include <SPI.h>
#include <Arduino.h>
#include <stdio.h>

SDLogger g_sdLogger;

static SPIClass sdSPI(VSPI);

SDLogger::SDLogger() : _ready(false) {}

bool SDLogger::begin() {
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN, sdSPI, 4000000)) {
        Serial.println("[SD] Card mount failed or not present");
        _ready = false;
        return false;
    }
    _ready = true;
    ensureHeader();
    Serial.println("[SD] Card ready");
    return true;
}

bool SDLogger::isReady() const { return _ready; }

bool SDLogger::ensureHeader() {
    if (!_ready) return false;
    // Only write header if file does not exist yet
    if (SD.exists(LOG_FILE)) return true;

    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] Cannot create log file");
        return false;
    }
    f.println("date_ms,exp_id,meas_idx,gain,mode,int_cycles,led_ma,"
              "ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,ch9,"
              "ch10,ch11,ch12,ch13,ch14,ch15,ch16,ch17,ch18");
    f.close();
    return true;
}

bool SDLogger::saveExperiment(const Experiment& exp) {
    if (!_ready) {
        Serial.println("[SD] Not ready, cannot save");
        return false;
    }

    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] Failed to open log for append");
        return false;
    }

    const SensorConfig& cfg = exp.sensor_cfg;
    for (int i = 0; i < exp.count; i++) {
        char line[256];
        int pos = snprintf(line, sizeof(line),
            "%u,%s,%d,%u,%u,%u,%u",
            exp.timestamp,
            exp.experiment_id,
            i,
            (uint8_t)cfg.gain,
            (uint8_t)cfg.mode,
            cfg.integrationCycles,
            cfg.ledCurrent);

        for (int j = 0; j < NUM_CHANNELS; j++) {
            pos += snprintf(line + pos, sizeof(line) - pos, ",%.4f", exp.spectra[i][j]);
        }
        line[pos++] = '\n';
        line[pos]   = '\0';
        f.print(line);
    }
    f.close();
    Serial.printf("[SD] Saved %d rows for exp '%s'\n", exp.count, exp.experiment_id);
    return true;
}

bool SDLogger::writeRow(const char* line) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (!f) return false;
    f.print(line);
    f.close();
    return true;
}

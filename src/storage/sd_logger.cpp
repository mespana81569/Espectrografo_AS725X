#include "sd_logger.h"
#include <SD.h>
#include <SPI.h>
#include <Arduino.h>
#include <time.h>

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

// ─── Helpers ─────────────────────────────────────────────────────────────────

static const char* gainStr(SensorGain g) {
    switch (g) {
        case SensorGain::GAIN_1X:  return "1x";
        case SensorGain::GAIN_4X:  return "4x";
        case SensorGain::GAIN_16X: return "16x";
        case SensorGain::GAIN_64X: return "64x";
        default: return "?";
    }
}

static void getDateStr(char* buf, size_t len) {
    time_t now = time(nullptr);
    if (now > 1600000000) {  // NTP synced (after Sept 2020)
        struct tm* t = localtime(&now);
        strftime(buf, len, "%Y-%m-%d %H:%M:%S", t);
    } else {
        // No NTP yet — use uptime
        unsigned long s = millis() / 1000;
        snprintf(buf, len, "boot+%lus", s);
    }
}

// ─── Header ──────────────────────────────────────────────────────────────────

bool SDLogger::ensureHeader() {
    if (!_ready) return false;

    // Check if file exists and has content
    if (SD.exists(LOG_FILE)) {
        File chk = SD.open(LOG_FILE, FILE_READ);
        if (chk && chk.size() > 0) { chk.close(); return true; }
        if (chk) chk.close();
    }

    File f = SD.open(LOG_FILE, FILE_WRITE);  // FILE_WRITE ok here — new file
    if (!f) {
        Serial.println("[SD] Cannot create log file");
        return false;
    }

    // Header: date, experiment info, config, calibration 18ch, measurement 18ch
    f.print("date,exp_id,meas_idx,gain,int_cycles,");
    f.print("white_led,white_mA,ir_led,ir_mA,uv_led,uv_mA,");
    f.print("cal_A_410,cal_B_435,cal_C_460,cal_D_485,cal_E_510,cal_F_535,");
    f.print("cal_G_560,cal_H_585,cal_I_610,cal_J_645,cal_K_680,cal_L_705,");
    f.print("cal_R_730,cal_S_760,cal_T_810,cal_U_860,cal_V_900,cal_W_940,");
    f.println("A_410,B_435,C_460,D_485,E_510,F_535,"
              "G_560,H_585,I_610,J_645,K_680,L_705,"
              "R_730,S_760,T_810,U_860,V_900,W_940");
    f.close();
    Serial.println("[SD] Created CSV with header");
    return true;
}

// ─── Save experiment ─────────────────────────────────────────────────────────

bool SDLogger::saveExperiment(const Experiment& exp) {
    if (!_ready) {
        Serial.println("[SD] Not ready, cannot save");
        return false;
    }

    // *** FILE_APPEND — critical: do NOT use FILE_WRITE, it truncates! ***
    File f = SD.open(LOG_FILE, FILE_APPEND);
    if (!f) {
        Serial.println("[SD] Failed to open log for append");
        return false;
    }

    char date[24];
    getDateStr(date, sizeof(date));

    const SensorConfig& cfg = exp.sensor_cfg;
    const CalibrationData& cal = exp.calibration;

    for (int i = 0; i < exp.count; i++) {
        // Date + experiment info
        f.print(date);           f.print(',');
        f.print(exp.experiment_id); f.print(',');
        f.print(i);             f.print(',');

        // Config — readable strings
        f.print(gainStr(cfg.gain)); f.print(',');
        f.print(cfg.integrationCycles); f.print(',');

        // LED config — ON/OFF + current
        f.print(cfg.ledWhiteEnabled ? "ON" : "OFF"); f.print(',');
        f.print(cfg.ledWhiteCurrent); f.print(',');
        f.print(cfg.ledIrEnabled ? "ON" : "OFF");    f.print(',');
        f.print(cfg.ledIrCurrent);    f.print(',');
        f.print(cfg.ledUvEnabled ? "ON" : "OFF");     f.print(',');
        f.print(cfg.ledUvCurrent);

        // Calibration data (18 channels)
        for (int j = 0; j < NUM_CHANNELS; j++) {
            f.print(',');
            if (cal.valid) f.print(cal.reference[j], 4);
            else           f.print(0.0f, 4);
        }

        // Measurement data (18 channels)
        for (int j = 0; j < NUM_CHANNELS; j++) {
            f.print(',');
            f.print(exp.spectra[i][j], 4);
        }

        f.println();
    }

    f.flush();
    f.close();
    Serial.printf("[SD] Saved %d rows for exp '%s'\n", exp.count, exp.experiment_id);
    return true;
}

// ─── Save calibration (separate file, for quick reference) ───────────────────

bool SDLogger::saveCalibration(const CalibrationData& cal) {
    if (!_ready) {
        Serial.println("[SD] Not ready, cannot save calibration");
        return false;
    }

    File f = SD.open(CAL_FILE, FILE_WRITE);  // overwrite is intentional here
    if (!f) {
        Serial.println("[SD] Failed to open calibration file");
        return false;
    }

    f.print("channel,wavelength,offset,reference\n");
    const uint16_t wl[] = {410,435,460,485,510,535,560,585,610,645,680,705,730,760,810,860,900,940};
    for (int i = 0; i < NUM_CHANNELS; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%d,%u,%.4f,%.4f\n", i, wl[i], cal.offset[i], cal.reference[i]);
        f.print(line);
    }

    f.close();
    Serial.println("[SD] Calibration saved");
    return true;
}

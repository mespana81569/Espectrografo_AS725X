#include "sd_logger.h"
#include <SD.h>
#include <SPI.h>
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
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
    if (!SD.exists(PENDING_DIR)) {
        SD.mkdir(PENDING_DIR);
    }
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

    // Mark experiment pending DB verification — must persist across reboots
    markPending(exp.experiment_id, exp.count);
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

// ─── Pending flag files ──────────────────────────────────────────────────────
//
// `/pending/<exp_id>.json` exists iff `exp_id` has been written to SD but
// NOT yet confirmed as stored in the DB. The file contents persist across
// reboots, so verification can resume after unexpected power loss.
//
// Flag file format:  {"exp_id":"EXP_001","expected_rows":5,"saved_at_ms":1234}
// ─────────────────────────────────────────────────────────────────────────────

static String pendingPath(const char* expId) {
    String p = PENDING_DIR;
    p += "/";
    p += expId;
    p += ".json";
    return p;
}

bool SDLogger::markPending(const char* expId, int expectedRows) {
    if (!_ready) return false;
    if (!SD.exists(PENDING_DIR)) SD.mkdir(PENDING_DIR);

    String path = pendingPath(expId);
    if (SD.exists(path)) SD.remove(path);

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[SD] Cannot create pending flag: %s\n", path.c_str());
        return false;
    }

    StaticJsonDocument<192> doc;
    doc["exp_id"] = expId;
    doc["expected_rows"] = expectedRows;
    doc["saved_at_ms"] = (uint32_t)millis();
    serializeJson(doc, f);
    f.close();
    Serial.printf("[SD] Pending flag written: %s (N=%d)\n", expId, expectedRows);
    return true;
}

bool SDLogger::clearPending(const char* expId) {
    if (!_ready) return false;
    String path = pendingPath(expId);
    if (!SD.exists(path)) return true;
    bool ok = SD.remove(path);
    if (ok) Serial.printf("[SD] Pending flag cleared: %s\n", expId);
    return ok;
}

bool SDLogger::readPendingFile(const char* path, char* expIdOut, size_t expIdLen, int* expectedOut) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    StaticJsonDocument<192> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[SD] Corrupt pending flag: %s (%s)\n", path, err.c_str());
        return false;
    }
    const char* id = doc["exp_id"] | "";
    if (strlen(id) == 0) return false;
    strncpy(expIdOut, id, expIdLen - 1);
    expIdOut[expIdLen - 1] = 0;
    *expectedOut = doc["expected_rows"] | 0;
    return true;
}

// ─── HTTP verification ───────────────────────────────────────────────────────

bool SDLogger::verifyWithServer(const char* expId, int expectedRows,
                                const char* host, uint16_t port) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[SD/verify] WiFi not connected");
        return false;
    }

    String url = "http://";
    url += host;
    url += ":";
    url += port;
    url += "/verify?exp_id=";
    url += expId;
    url += "&expected=";
    url += expectedRows;

    for (int attempt = 1; attempt <= VERIFY_MAX_RETRIES; attempt++) {
        HTTPClient http;
        http.setTimeout(VERIFY_TIMEOUT_MS);
        if (!http.begin(url)) {
            Serial.printf("[SD/verify] HTTP begin failed (attempt %d)\n", attempt);
            delay(500);
            continue;
        }
        int code = http.GET();
        if (code == 200) {
            String body = http.getString();
            http.end();

            StaticJsonDocument<256> doc;
            if (deserializeJson(doc, body)) {
                Serial.printf("[SD/verify] Bad JSON for %s: %s\n", expId, body.c_str());
                delay(500);
                continue;
            }
            bool verified = doc["verified"] | false;
            int found = doc["rows_found"] | 0;
            int expected = doc["rows_expected"] | 0;
            Serial.printf("[SD/verify] %s: verified=%d found=%d/%d\n",
                          expId, verified ? 1 : 0, found, expected);
            return verified;
        } else {
            Serial.printf("[SD/verify] HTTP %d on attempt %d for %s\n",
                          code, attempt, expId);
            http.end();
            delay(500);
        }
    }
    return false;
}

// ─── Filtered CSV rewrite ────────────────────────────────────────────────────
//
// Atomically rewrites LOG_FILE without rows whose exp_id (CSV column 2)
// matches. Writes to LOG_FILE.tmp, then swaps. On any failure the tmp is
// removed and the original file is untouched — NEVER data-lossy.
// ─────────────────────────────────────────────────────────────────────────────

bool SDLogger::removeExperimentRows(const char* expId) {
    if (!_ready) return false;
    if (!SD.exists(LOG_FILE)) return true;

    const char* tmpPath = "/spectra.tmp";
    if (SD.exists(tmpPath)) SD.remove(tmpPath);

    File in = SD.open(LOG_FILE, FILE_READ);
    if (!in) {
        Serial.println("[SD/remove] Cannot open source log");
        return false;
    }
    File out = SD.open(tmpPath, FILE_WRITE);
    if (!out) {
        in.close();
        Serial.println("[SD/remove] Cannot create tmp log");
        return false;
    }

    // Header: copy verbatim
    String header = in.readStringUntil('\n');
    if (header.length() > 0) {
        out.print(header);
        out.print('\n');
    }

    const size_t idLen = strlen(expId);
    int kept = 0, dropped = 0;

    while (in.available()) {
        String line = in.readStringUntil('\n');
        if (line.length() == 0) continue;

        // exp_id is the 2nd column (index 1). Find first comma, then scan
        // the next column up to the second comma.
        int firstComma = line.indexOf(',');
        if (firstComma < 0) { out.print(line); out.print('\n'); kept++; continue; }
        int secondComma = line.indexOf(',', firstComma + 1);
        if (secondComma < 0) { out.print(line); out.print('\n'); kept++; continue; }

        // Compare without allocating a substring
        bool match = ((size_t)(secondComma - firstComma - 1) == idLen) &&
                     (memcmp(line.c_str() + firstComma + 1, expId, idLen) == 0);

        if (match) {
            dropped++;
        } else {
            out.print(line);
            out.print('\n');
            kept++;
        }
    }

    in.close();
    out.flush();
    out.close();

    if (dropped == 0) {
        // Nothing to do; discard tmp to avoid touching the source file
        SD.remove(tmpPath);
        Serial.printf("[SD/remove] No rows for %s — unchanged\n", expId);
        return true;
    }

    // Atomic swap: remove original, rename tmp.
    if (!SD.remove(LOG_FILE)) {
        Serial.println("[SD/remove] Cannot remove original; keeping tmp for retry");
        return false;
    }
    if (!SD.rename(tmpPath, LOG_FILE)) {
        Serial.println("[SD/remove] Rename failed — data may be in tmp");
        return false;
    }
    Serial.printf("[SD/remove] %s: dropped %d rows, kept %d\n", expId, dropped, kept);
    return true;
}

// ─── Cleanup pass ────────────────────────────────────────────────────────────
//
// For every flag file in /pending, contact the DB-backed server and delete
// SD rows only after {"verified":true}. On any error the flag file stays,
// so the next cleanup pass retries. Safe across reboots.
// ─────────────────────────────────────────────────────────────────────────────

int SDLogger::cleanupVerifiedExperiments(const char* host, uint16_t port) {
    if (!_ready) return 0;
    if (WiFi.status() != WL_CONNECTED) return 0;
    if (!SD.exists(PENDING_DIR)) return 0;

    File dir = SD.open(PENDING_DIR);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }

    int cleaned = 0;
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) { entry.close(); continue; }

        String name = entry.name();
        // SD library on ESP32 returns either "/pending/EXP_001.json" or
        // just the basename depending on library version. Normalize.
        int slash = name.lastIndexOf('/');
        String base = (slash >= 0) ? name.substring(slash + 1) : name;
        String fullPath = PENDING_DIR;
        fullPath += "/";
        fullPath += base;
        entry.close();

        if (!base.endsWith(".json")) continue;

        char expId[32] = {0};
        int expected = 0;
        if (!readPendingFile(fullPath.c_str(), expId, sizeof(expId), &expected)) continue;
        if (expected <= 0 || strlen(expId) == 0) continue;

        if (!verifyWithServer(expId, expected, host, port)) {
            // Not yet verified — keep the flag; next pass will retry
            continue;
        }

        if (!removeExperimentRows(expId)) {
            Serial.printf("[SD/cleanup] Verified but CSV rewrite failed: %s\n", expId);
            continue;  // Keep flag; retry next pass
        }

        if (!clearPending(expId)) {
            Serial.printf("[SD/cleanup] Rows dropped but flag clear failed: %s\n", expId);
            // Non-fatal — next pass finds flag, re-verifies (still true),
            // re-runs removeExperimentRows (no-op since rows already gone),
            // re-tries clearPending.
        }
        cleaned++;
    }
    dir.close();

    if (cleaned > 0) {
        Serial.printf("[SD/cleanup] %d experiment(s) verified and purged\n", cleaned);
    }
    return cleaned;
}

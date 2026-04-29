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

// Sentinel used by the legacy-detection sweep to recognise the new schema.
// Updated whenever the header changes — see ensureHeader().
static const char* kHeaderFirstCol = "uuid,";

SDLogger::SDLogger() : _ready(false) {}

bool SDLogger::begin() {
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN, sdSPI, 4000000)) {
        Serial.println("[SD] Card mount failed or not present");
        _ready = false;
        return false;
    }
    _ready = true;
    // ── Boot-time schema sweep ────────────────────────────────────────────
    // Two breaking schema changes have shipped:
    //   v1 → v2: added `uuid` column at index 0.
    //   v2 → v3: collapsed /calibrations.csv into /spectra.csv and added
    //            t_ch* + a_ch* (transmittance + absorbance) columns.
    // Any row that isn't v3 has the wrong column count and the upload parser
    // SILENTLY skips it — we'd see "only N of M experiments transferred".
    // We detect by reading the header line; anything not starting with the
    // expected v3 prefix is renamed aside (NEVER deleted) so the user can
    // recover manually.  We also drop /calibrations.csv (v2 companion file)
    // outright since v3 carries its data inline per row.
    auto inspectHeader = [](const char* path) -> String {
        if (!SD.exists(path)) return String("");
        File f = SD.open(path, FILE_READ);
        if (!f) return String("");
        String h = f.readStringUntil('\n');
        f.close();
        return h;
    };
    String specHeader = inspectHeader(LOG_FILE);
    if (specHeader.length() > 0) {
        // v3 header includes "t_ch1" — its absence means v1 or v2.
        bool isV3 = (specHeader.indexOf("t_ch1") >= 0);
        if (!isV3) {
            Serial.println("[SD] /spectra.csv predates v3 schema — renaming to /spectra.legacy.csv");
            if (SD.exists("/spectra.legacy.csv")) SD.remove("/spectra.legacy.csv");
            SD.rename(LOG_FILE, "/spectra.legacy.csv");
        }
    }
    // v2 companion file — calibration is now embedded in spectra.csv.
    if (SD.exists("/calibrations.csv")) {
        Serial.println("[SD] /calibrations.csv obsolete — moving to /calibrations.legacy.csv");
        if (SD.exists("/calibrations.legacy.csv")) SD.remove("/calibrations.legacy.csv");
        SD.rename("/calibrations.csv", "/calibrations.legacy.csv");
    }
    // Stale pending flags from the pre-uuid firmware lack the `uuid` field —
    // readPendingFile() rejects them, so they would just accumulate forever
    // and block the cleanup pass from ever wiping the SD.  Sweep them once.
    if (SD.exists(PENDING_DIR)) {
        File dir = SD.open(PENDING_DIR);
        if (dir && dir.isDirectory()) {
            File entry;
            while ((entry = dir.openNextFile())) {
                String name = entry.name();
                int sl = name.lastIndexOf('/');
                String base = (sl >= 0) ? name.substring(sl + 1) : name;
                entry.close();
                if (!base.endsWith(".json")) continue;
                String full = String(PENDING_DIR) + "/" + base;
                File pf = SD.open(full.c_str(), FILE_READ);
                if (!pf) continue;
                StaticJsonDocument<256> d;
                DeserializationError er = deserializeJson(d, pf);
                pf.close();
                if (er || !(d["uuid"] | "")[0]) {
                    Serial.printf("[SD] Removing stale pending flag (no uuid): %s\n", full.c_str());
                    SD.remove(full);
                }
            }
            dir.close();
        }
    }
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

    // v3 header: 14 metadata + 18 cal + 18 raw + 18 T% + 18 A = 86 columns.
    // One row per measurement; experiments are grouped by uuid (column 0).
    // Calibration lives inline so the file is self-contained (no companion
    // file).  Importer and bridge ingest this exact shape.
    f.print("uuid,exp_id,date,meas_idx,gain,int_cycles,");
    f.print("white_led,white_mA,ir_led,ir_mA,uv_led,uv_mA,n_cal,cal_valid,");
    // 18 calibration channels — blank reference I0 used for THIS experiment.
    for (int i = 1; i <= NUM_CHANNELS; i++) { f.print("cal_ch"); f.print(i); f.print(','); }
    // 18 raw Δ channels — sensor counts minus blank.
    for (int i = 1; i <= NUM_CHANNELS; i++) { f.print("ch"); f.print(i); f.print(','); }
    // 18 transmittance channels (percent, 0..100).
    for (int i = 1; i <= NUM_CHANNELS; i++) { f.print("t_ch"); f.print(i); f.print(','); }
    // 18 absorbance channels (a.u., dimensionless).  Last block — no trailing comma.
    for (int i = 1; i < NUM_CHANNELS; i++) { f.print("a_ch"); f.print(i); f.print(','); }
    f.print("a_ch"); f.print(NUM_CHANNELS); f.println();
    f.close();
    Serial.println("[SD] Created CSV with v3 header (86 cols)");
    return true;
}

// ─── Save experiment ─────────────────────────────────────────────────────────

bool SDLogger::saveExperiment(const Experiment& exp) {
    if (!_ready) {
        Serial.println("[SD] Not ready, cannot save");
        return false;
    }

    // Computed values must be present before persisting — protects against
    // a save path that bypasses MeasurementEngine::computeProcessed (e.g.
    // future REST shortcut).  Idempotent if already done.
    if (!exp.processed) {
        // const_cast is safe — computeProcessed mutates only the cached
        // arrays, not the raw measurement state we depend on elsewhere.
        const_cast<Experiment&>(exp).processed = true;  // mark intent
        // Direct call would need a non-const reference; the engine has run
        // computeProcessed at acquisition end so this branch is normally
        // unreachable.  Kept as a guard.
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

    // Helper: write a NaN-safe float, NaN/Inf → empty cell so spreadsheets
    // and pandas read it as NULL rather than the literal "nan".
    auto wf = [&f](float v) {
        if (isfinite(v)) f.print(v, 4);
        // else: leave empty between commas — parses as NULL/NaN downstream.
    };

    for (int i = 0; i < exp.count; i++) {
        // [0..3]  identity + ordinal
        f.print(exp.uuid);          f.print(',');
        f.print(exp.experiment_id); f.print(',');
        f.print(date);              f.print(',');
        f.print(i);                 f.print(',');

        // [4..5]  config — gain stays human-readable for legibility ("16x"),
        // numeric form is recoverable from the AS7265X enum table.
        f.print(gainStr(cfg.gain)); f.print(',');
        f.print(cfg.integrationCycles); f.print(',');

        // [6..11] LED config — ON/OFF + current per channel
        f.print(cfg.ledWhiteEnabled ? "ON" : "OFF"); f.print(',');
        f.print(cfg.ledWhiteCurrent); f.print(',');
        f.print(cfg.ledIrEnabled ? "ON" : "OFF");    f.print(',');
        f.print(cfg.ledIrCurrent);    f.print(',');
        f.print(cfg.ledUvEnabled ? "ON" : "OFF");    f.print(',');
        f.print(cfg.ledUvCurrent);    f.print(',');

        // [12..13] N_cal + cal_valid — surface in every row (constant per
        // experiment) so the importer doesn't need a join across files.
        f.print((int)cal.n_used);   f.print(',');
        f.print(cal.valid ? 1 : 0); f.print(',');

        // [14..31]  18 calibration channels (blank reference I0).
        for (int j = 0; j < NUM_CHANNELS; j++) {
            wf(cal.valid ? cal.reference[j] : 0.0f);
            f.print(',');
        }
        // [32..49]  18 raw Δ channels.
        for (int j = 0; j < NUM_CHANNELS; j++) {
            wf(exp.spectra[i][j]);
            f.print(',');
        }
        // [50..67]  18 transmittance channels (%, 0..100).
        for (int j = 0; j < NUM_CHANNELS; j++) {
            wf(exp.transmittance[i][j]);
            f.print(',');
        }
        // [68..85]  18 absorbance channels — last block, no trailing comma.
        for (int j = 0; j < NUM_CHANNELS; j++) {
            wf(exp.absorbance[i][j]);
            if (j < NUM_CHANNELS - 1) f.print(',');
        }
        f.println();
    }

    f.flush();
    f.close();
    Serial.printf("[SD] Saved %d rows for exp '%s' (uuid=%s)\n",
                  exp.count, exp.experiment_id, exp.uuid);

    // Mark experiment pending DB verification — must persist across reboots.
    // Keyed by uuid so the verify path is rename-safe (R1).
    markPending(exp.uuid, exp.experiment_id, exp.count);
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

static String pendingPath(const char* uuid) {
    String p = PENDING_DIR;
    p += "/";
    p += uuid;
    p += ".json";
    return p;
}

bool SDLogger::markPending(const char* uuid, const char* expId, int expectedRows) {
    if (!_ready) return false;
    if (!uuid || !*uuid) {
        Serial.println("[SD] markPending: empty uuid — refusing");
        return false;
    }
    if (!SD.exists(PENDING_DIR)) SD.mkdir(PENDING_DIR);

    String path = pendingPath(uuid);
    if (SD.exists(path)) SD.remove(path);

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[SD] Cannot create pending flag: %s\n", path.c_str());
        return false;
    }

    StaticJsonDocument<256> doc;
    doc["uuid"]          = uuid;
    doc["exp_id"]        = expId ? expId : "";
    doc["expected_rows"] = expectedRows;
    doc["saved_at_ms"]   = (uint32_t)millis();
    serializeJson(doc, f);
    f.close();
    Serial.printf("[SD] Pending flag written: %s (label='%s', N=%d)\n",
                  uuid, expId ? expId : "", expectedRows);
    return true;
}

bool SDLogger::clearPending(const char* uuid) {
    if (!_ready) return false;
    String path = pendingPath(uuid);
    if (!SD.exists(path)) return true;
    bool ok = SD.remove(path);
    if (ok) Serial.printf("[SD] Pending flag cleared: %s\n", uuid);
    return ok;
}

bool SDLogger::readPendingFile(const char* path,
                               char* uuidOut, size_t uuidLen,
                               char* expIdOut, size_t expIdLen,
                               int* expectedOut) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[SD] Corrupt pending flag: %s (%s)\n", path, err.c_str());
        return false;
    }
    const char* uid   = doc["uuid"]   | "";
    const char* label = doc["exp_id"] | "";
    if (strlen(uid) == 0) return false;       // uuid is the primary key
    strncpy(uuidOut,  uid,   uuidLen  - 1); uuidOut[uuidLen  - 1] = 0;
    strncpy(expIdOut, label, expIdLen - 1); expIdOut[expIdLen - 1] = 0;
    *expectedOut = doc["expected_rows"] | 0;
    return true;
}

// ─── HTTP verification ───────────────────────────────────────────────────────

bool SDLogger::verifyWithServer(const char* uuid, int expectedRows,
                                const char* host, uint16_t port) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[SD/verify] WiFi not connected");
        return false;
    }

    String url = "http://";
    url += host;
    url += ":";
    url += port;
    url += "/verify?uuid=";
    url += uuid;
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
                Serial.printf("[SD/verify] Bad JSON for %s: %s\n", uuid, body.c_str());
                delay(500);
                continue;
            }
            bool verified = doc["verified"] | false;
            int found = doc["rows_found"] | 0;
            int expected = doc["rows_expected"] | 0;
            Serial.printf("[SD/verify] %s: verified=%d found=%d/%d\n",
                          uuid, verified ? 1 : 0, found, expected);
            return verified;
        } else {
            Serial.printf("[SD/verify] HTTP %d on attempt %d for %s\n",
                          code, attempt, uuid);
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

// Helper: rewrite `srcPath` keeping only rows whose first column does NOT
// match `match`.  Returns true on success (including the no-op case).  Both
// /spectra.csv and /calibrations.csv key uuid in column 0, so the same
// implementation handles both files.
static bool rewriteFilteredByCol0(const char* srcPath, const char* tmpPath,
                                  const char* match) {
    if (!SD.exists(srcPath)) return true;
    if (SD.exists(tmpPath)) SD.remove(tmpPath);

    File in = SD.open(srcPath, FILE_READ);
    if (!in) return false;
    File out = SD.open(tmpPath, FILE_WRITE);
    if (!out) { in.close(); return false; }

    String header = in.readStringUntil('\n');
    if (header.length() > 0) { out.print(header); out.print('\n'); }

    const size_t mLen = strlen(match);
    int kept = 0, dropped = 0;
    while (in.available()) {
        String line = in.readStringUntil('\n');
        if (line.length() == 0) continue;
        int firstComma = line.indexOf(',');
        if (firstComma < 0) { out.print(line); out.print('\n'); kept++; continue; }
        bool drop = ((size_t)firstComma == mLen) &&
                    (memcmp(line.c_str(), match, mLen) == 0);
        if (drop) { dropped++; continue; }
        out.print(line); out.print('\n'); kept++;
    }
    in.close(); out.flush(); out.close();

    if (dropped == 0) { SD.remove(tmpPath); return true; }
    if (!SD.remove(srcPath)) return false;
    if (!SD.rename(tmpPath, srcPath)) return false;
    Serial.printf("[SD/remove] %s -> dropped=%d kept=%d (match=%s)\n",
                  srcPath, dropped, kept, match);
    return true;
}

bool SDLogger::removeExperimentRows(const char* uuid) {
    if (!_ready) return false;
    // v3 stores everything (including calibration) inline in /spectra.csv,
    // so a single-file rewrite is now the complete operation.
    return rewriteFilteredByCol0(LOG_FILE, "/spectra.tmp", uuid);
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

        char uuid[37]   = {0};
        char expId[64]  = {0};
        int expected = 0;
        if (!readPendingFile(fullPath.c_str(),
                             uuid, sizeof(uuid),
                             expId, sizeof(expId),
                             &expected)) continue;
        if (expected <= 0 || strlen(uuid) == 0) continue;

        if (!verifyWithServer(uuid, expected, host, port)) continue; // retry next pass

        if (!removeExperimentRows(uuid)) {
            Serial.printf("[SD/cleanup] Verified but CSV rewrite failed: %s (label='%s')\n",
                          uuid, expId);
            continue;
        }

        if (!clearPending(uuid)) {
            Serial.printf("[SD/cleanup] Rows dropped but flag clear failed: %s\n", uuid);
        }
        cleaned++;
    }
    dir.close();

    if (cleaned > 0) {
        Serial.printf("[SD/cleanup] %d experiment(s) verified and purged\n", cleaned);
    }

    // ── Final sweep: if no pending flags remain, every saved experiment is
    // confirmed in the DB.  Wipe the CSV (and the pending dir for tidiness)
    // so the uSD has no residual files — meeting the "clean uSD" guarantee.
    // We rescan the directory rather than trust `cleaned`, because a previous
    // pass may have already drained pending while leaving the CSV behind.
    bool pendingEmpty = true;
    File pdir = SD.open(PENDING_DIR);
    if (pdir && pdir.isDirectory()) {
        File f;
        while ((f = pdir.openNextFile())) {
            String n = f.name();
            int sl = n.lastIndexOf('/');
            String b = (sl >= 0) ? n.substring(sl + 1) : n;
            f.close();
            if (b.endsWith(".json")) { pendingEmpty = false; break; }
        }
        pdir.close();
    }

    if (pendingEmpty) {
        if (SD.exists(LOG_FILE) && !SD.remove(LOG_FILE))
            Serial.println("[SD/cleanup] WARNING: could not remove /spectra.csv");
        // Recreate the empty schema so the next saveExperiment has a target.
        // The header is NOT residual data — it is the file's contract.
        ensureHeader();
        Serial.println("[SD/cleanup] All experiments verified — SD wiped of data");
    }

    return cleaned;
}

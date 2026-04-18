#pragma once
#include "../acquisition/measurement_engine.h"
#include "../acquisition/calibration.h"
#include <Arduino.h>

#define SD_CS_PIN   5
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#define SD_SCK_PIN  18
#define LOG_FILE    "/spectra.csv"
#define CAL_FILE    "/calibration.csv"
#define PENDING_DIR "/pending"

// HTTP verify retry policy (applied per pending experiment per cleanup pass)
#define VERIFY_MAX_RETRIES   3
#define VERIFY_TIMEOUT_MS    5000

class SDLogger {
public:
    SDLogger();
    bool begin();
    bool isReady() const;

    // Save full experiment to SD (APPENDS CSV rows — never truncates).
    // On success, also writes a pending flag file so the data can later be
    // verified against the DB and deleted from SD.
    bool saveExperiment(const Experiment& exp);

    // Save calibration data to SD (overwrites previous)
    bool saveCalibration(const CalibrationData& cal);

    // Write CSV header if file is new or empty
    bool ensureHeader();

    // ── Verify-before-delete pattern ─────────────────────────────────────
    // Writes /pending/<expId>.json with {exp_id, expected_rows, saved_at_ms}.
    // Safe to call multiple times (overwrites).
    bool markPending(const char* expId, int expectedRows);

    // Remove /pending/<expId>.json (called after verified deletion).
    bool clearPending(const char* expId);

    // HTTP GET http://<host>:<port>/verify?exp_id=...&expected=N
    // Returns true only when server response is {"verified":true,...}.
    // Blocking; uses HTTPClient; retries up to VERIFY_MAX_RETRIES times.
    bool verifyWithServer(const char* expId, int expectedRows,
                          const char* host, uint16_t port);

    // Rewrite LOG_FILE excluding rows whose exp_id column matches `expId`.
    // Atomic: writes to LOG_FILE + ".tmp" then renames on success.
    bool removeExperimentRows(const char* expId);

    // Iterate PENDING_DIR, verify each experiment with the server, and on
    // verified:true remove the rows from LOG_FILE and delete the flag file.
    // Returns number of experiments successfully cleaned up in this pass.
    // Call this periodically from loop() while STA WiFi is connected.
    int cleanupVerifiedExperiments(const char* host, uint16_t port);

private:
    bool _ready;
    bool readPendingFile(const char* path, char* expIdOut, size_t expIdLen, int* expectedOut);
};

extern SDLogger g_sdLogger;

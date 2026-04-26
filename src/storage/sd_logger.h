#pragma once
#include "../acquisition/measurement_engine.h"
#include "../acquisition/calibration.h"
#include <Arduino.h>

#define SD_CS_PIN   5
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#define SD_SCK_PIN  18
#define LOG_FILE    "/spectra.csv"
// Per-experiment append-only calibration file, keyed by uuid.  The previous
// /calibration.csv was a single overwriting file — fundamentally incompatible
// with R6 (every experiment binds its own calibration explicitly).  Removed.
#define CAL_FILE    "/calibrations.csv"
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
    // verified against the DB and deleted from SD.  ALSO appends one
    // matching row to /calibrations.csv keyed by exp.uuid (R6).
    bool saveExperiment(const Experiment& exp);

    // Append one calibration row, keyed by uuid + exp_id.  No overwriting:
    // the file accumulates one row per experiment so the device can ship
    // its full SD layout to the dashboard for manual import (issue #8).
    bool appendCalibration(const Experiment& exp);

    // Write CSV header if file is new or empty
    bool ensureHeader();

    // ── Verify-before-delete pattern ─────────────────────────────────────
    // Writes /pending/<uuid>.json with {uuid, exp_id, expected_rows, saved_at_ms}.
    // Keyed by uuid (R1) so a rename of exp_id between save and verify still
    // resolves to the right pending flag.  Safe to call multiple times.
    bool markPending(const char* uuid, const char* expId, int expectedRows);
    bool clearPending(const char* uuid);

    // HTTP GET http://<host>:<port>/verify?uuid=...&expected=N
    bool verifyWithServer(const char* uuid, int expectedRows,
                          const char* host, uint16_t port);

    // Rewrite LOG_FILE excluding rows whose uuid (CSV column 0) matches.
    // Atomic: writes to LOG_FILE + ".tmp" then renames on success.  Also
    // strips the matching uuid from /calibrations.csv so the SD layout
    // stays consistent (no orphan calibration row pointing at deleted data).
    bool removeExperimentRows(const char* uuid);

    // Iterate PENDING_DIR, verify each experiment with the server, and on
    // verified:true remove the rows from LOG_FILE and delete the flag file.
    // Returns number of experiments successfully cleaned up in this pass.
    // Call this periodically from loop() while STA WiFi is connected.
    int cleanupVerifiedExperiments(const char* host, uint16_t port);

private:
    bool _ready;
    // pending JSON now carries both uuid and exp_id; readPendingFile fills
    // both so the cleanup pass can call /verify and report human-friendly
    // labels in the serial log.
    bool readPendingFile(const char* path,
                         char* uuidOut, size_t uuidLen,
                         char* expIdOut, size_t expIdLen,
                         int* expectedOut);
    // Strip rows from /calibrations.csv where uuid column matches.  Same
    // atomic-rewrite contract as removeExperimentRows().
    bool removeCalibrationRow(const char* uuid);
};

extern SDLogger g_sdLogger;

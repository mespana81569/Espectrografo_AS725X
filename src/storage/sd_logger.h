#pragma once
#include "../acquisition/measurement_engine.h"
#include "../acquisition/calibration.h"
#include <Arduino.h>

#define SD_CS_PIN   5
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#define SD_SCK_PIN  18
// Single consolidated experiment file.  One row per measurement, all data
// bound to the row: experiment metadata + calibration vector + raw Δ +
// transmittance % + absorbance.  No companion file — calibration travels
// inline so the file is self-contained and parseable in one pass.  An
// "archive" (= the raw /spectra.csv) can hold 1..N experiments, grouped by
// uuid.  Bridge and importer ingest that same shape.
#define LOG_FILE    "/spectra.csv"
#define PENDING_DIR "/pending"

// HTTP verify retry policy (applied per pending experiment per cleanup pass)
#define VERIFY_MAX_RETRIES   3
#define VERIFY_TIMEOUT_MS    5000

class SDLogger {
public:
    SDLogger();
    bool begin();
    bool isReady() const;

    // Save full experiment to SD (APPENDS rows — never truncates).
    // Each row carries: metadata + 18 cal + 18 raw Δ + 18 T% + 18 A.
    // On success, also writes a pending flag file so the data can later be
    // verified against the DB and deleted from SD.
    bool saveExperiment(const Experiment& exp);

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
    // Atomic: writes to LOG_FILE + ".tmp" then renames on success.
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
};

extern SDLogger g_sdLogger;

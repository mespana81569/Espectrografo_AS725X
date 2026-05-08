#include <Arduino.h>
#include <WiFi.h>
#include "core/state_machine.h"
#include "sensors/as7265x_driver.h"
#include "acquisition/measurement_engine.h"
#include "acquisition/calibration.h"
#include "storage/sd_logger.h"
#include "web/web_server.h"
#include "mqtt/mqtt_client.h"

// ── DB-side host/port for /verify cleanup pass ──────────────────────────────
// The Flask service that exposes /verify lives on the same machine as the
// MQTT broker (docker-compose stack).  Keep these aligned with mqtt_client.h
// — if the broker host changes, this must too.
#define DB_VERIFY_HOST "192.168.1.59"
#define DB_VERIFY_PORT 5000

// Period between SD → DB verification passes.  Each pass blocks for up to
// ~15s per pending experiment (HTTPClient retries), so don't run it too
// often.  30 s is a balance between responsiveness after a pull_data and
// not starving the rest of the loop.
static const unsigned long CLEANUP_INTERVAL_MS = 30000;
static unsigned long s_lastCleanupMs = 0;

// Live monitor shared buffer — read by GET /api/monitor
float    g_liveBuf[18] = {0};
volatile bool g_liveReady = false;

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[Boot] Spectrograph AS7265X starting...");

    // Initialize sensor
    if (!g_sensorDriver.begin()) {
        Serial.println("[Boot] WARNING: Sensor not found — continuing without sensor");
    }

    // Initialize SD card
    if (!g_sdLogger.begin()) {
        Serial.println("[Boot] WARNING: SD card not available — logging disabled");
    }

    // Initialize default experiment
    g_measurementEngine.resetExperiment("EXP_001");
    g_measurementEngine.configure(5, g_sensorDriver.getConfig());

    // Start Wi-Fi AP + HTTP server
    webServerSetup();

    // MQTT client — connects in tick() once STA link is up.
    g_mqttClient.begin();

    Serial.println("[Boot] System ready");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    // Tick the state machine — handles transitions and in-state logic
    g_stateMachine.tick();

    SystemState st = g_stateMachine.getState();

    // Drive calibration process while in CALIBRATION state
    if (st == SystemState::CALIBRATION) {
        g_calibration.tick();
    }

    // Drive acquisition while in MEASUREMENT state
    if (st == SystemState::MEASUREMENT) {
        g_measurementEngine.tick();
    }

    // Drive live monitor — take continuous measurements into shared buffer
    if (st == SystemState::LIVE_MONITOR) {
        // takeMeasurement() is blocking (~integration time), so it self-throttles
        if (g_sensorDriver.takeMeasurement(g_liveBuf)) {
            g_liveReady = true;
        }
    } else {
        g_liveReady = false;
    }

    // Periodic web tasks (WiFi STA connection state machine)
    webServerLoop();

    // MQTT: reconnect, pump protocol, process commands, drive bulk upload.
    g_mqttClient.tick();

    // ── Periodic SD → DB verify-and-purge pass ───────────────────────────
    // This is the last leg of the upload protocol:
    //   1. saveExperiment() writes CSV + creates /pending/<exp>.json   (immediate)
    //   2. user-triggered MQTT bulk upload publishes every CSV row     (pull_data)
    //   3. mqtt_to_db.py inserts into MySQL                            (server-side)
    //   4. cleanupVerifiedExperiments() HTTP-GETs /verify, drops rows  (here)
    //
    // Step 4 is what makes deletion SAFE: a row is removed from the CSV
    // only after the server confirms that ≥ expected_rows are present in
    // MySQL.  Anything that fails to verify keeps its pending flag and its
    // CSV rows, so the next pull_data re-publishes pending + new data.
    // After the last pending flag is cleared, cleanupVerifiedExperiments
    // also wipes /spectra.csv so the uSD has no residual data files.
    //
    // Skip while: WiFi STA not connected, MEASUREMENT/CALIBRATION in flight
    // (don't steal SD bandwidth), or an MQTT bulk upload is still draining.
    if (WiFi.status() == WL_CONNECTED &&
        st != SystemState::CALIBRATION &&
        st != SystemState::MEASUREMENT &&
        st != SystemState::LIVE_MONITOR) {
        unsigned long now = millis();
        if (now - s_lastCleanupMs >= CLEANUP_INTERVAL_MS) {
            s_lastCleanupMs = now;
            g_sdLogger.cleanupVerifiedExperiments(DB_VERIFY_HOST, DB_VERIFY_PORT);
        }
    }
}

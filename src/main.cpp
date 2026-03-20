#include <Arduino.h>
#include "core/state_machine.h"
#include "sensors/as7265x_driver.h"
#include "acquisition/measurement_engine.h"
#include "acquisition/calibration.h"
#include "storage/sd_logger.h"
#include "web/web_server.h"

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

    Serial.println("[Boot] System ready");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    // Tick the state machine — handles transitions and in-state logic
    g_stateMachine.tick();

    // Drive calibration process while in CALIBRATION state
    if (g_stateMachine.getState() == SystemState::CALIBRATION) {
        g_calibration.tick();
    }

    // Drive acquisition while in MEASUREMENT state
    if (g_stateMachine.getState() == SystemState::MEASUREMENT) {
        g_measurementEngine.tick();
    }

    // Periodic web tasks (mDNS etc.)
    webServerLoop();

    delay(10);
}

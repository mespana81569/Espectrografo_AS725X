#include <Arduino.h>
#include "core/state_machine.h"
#include "sensors/as7265x_driver.h"
#include "acquisition/measurement_engine.h"
#include "acquisition/calibration.h"
#include "storage/sd_logger.h"
#include "web/web_server.h"
#include "mqtt/mqtt_client.h"

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
}

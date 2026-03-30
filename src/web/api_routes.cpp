#include "api_routes.h"
#include "web_server.h"
#include "../core/state_machine.h"
#include "../sensors/as7265x_driver.h"
#include "../acquisition/measurement_engine.h"
#include "../acquisition/calibration.h"
#include "../storage/sd_logger.h"
#include <Arduino.h>
#include <ArduinoJson.h>

// ─── Helpers ────────────────────────────────────────────────────────────────

static void sendJson(AsyncWebServerRequest* req, const String& body, int code = 200) {
    req->send(code, "application/json", body);
}

static void sendOk(AsyncWebServerRequest* req, const char* msg = "ok") {
    sendJson(req, String("{\"status\":\"") + msg + "\"}");
}

static void sendError(AsyncWebServerRequest* req, const char* msg, int code = 400) {
    sendJson(req, String("{\"error\":\"") + msg + "\"}", code);
}

// ─── GET /api/status ─────────────────────────────────────────────────────────
// Returns current state, sensor ready flag, SD ready flag, calibration valid
static void handleGetStatus(AsyncWebServerRequest* req) {
    StaticJsonDocument<256> doc;
    doc["state"]       = g_stateMachine.getStateName();
    doc["sensorReady"] = g_sensorDriver.isReady();
    doc["sdReady"]     = g_sdLogger.isReady();
    doc["calValid"]    = g_calibration.getData().valid;
    doc["measCount"]   = g_measurementEngine.getExperiment().count;
    doc["measTarget"]  = g_measurementEngine.getExperiment().num_measurements;
    String out;
    serializeJson(doc, out);
    sendJson(req, out);
}

// ─── GET /api/config ─────────────────────────────────────────────────────────
static void handleGetConfig(AsyncWebServerRequest* req) {
    SensorConfig cfg = g_sensorDriver.getConfig();
    StaticJsonDocument<128> doc;
    doc["gain"]             = (uint8_t)cfg.gain;
    doc["integrationCycles"] = cfg.integrationCycles;
    doc["mode"]             = (uint8_t)cfg.mode;
    doc["ledCurrent"]       = cfg.ledCurrent;
    doc["ledEnabled"]       = cfg.ledEnabled;
    String out;
    serializeJson(doc, out);
    sendJson(req, out);
}

// ─── POST /api/config ────────────────────────────────────────────────────────
// Body: JSON with gain, integrationCycles, mode, ledCurrent, ledEnabled, N, expId
static void handleSetConfig(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
    if (g_stateMachine.getState() != SystemState::IDLE) {
        sendError(req, "Config only allowed in IDLE state");
        return;
    }
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) { sendError(req, "Invalid JSON"); return; }

    SensorConfig cfg = g_sensorDriver.getConfig();
    if (doc.containsKey("gain"))              cfg.gain             = (SensorGain)(uint8_t)doc["gain"];
    if (doc.containsKey("integrationCycles")) cfg.integrationCycles = doc["integrationCycles"];
    if (doc.containsKey("mode"))              cfg.mode             = (MeasurementMode)(uint8_t)doc["mode"];
    if (doc.containsKey("ledCurrent"))        cfg.ledCurrent       = doc["ledCurrent"];
    if (doc.containsKey("ledEnabled"))        cfg.ledEnabled       = doc["ledEnabled"];

    g_sensorDriver.applyConfig(cfg);

    int n = doc.containsKey("N") ? (int)doc["N"] : 5;
    g_measurementEngine.configure(n, cfg);

    if (doc.containsKey("expId")) {
        g_measurementEngine.resetExperiment(doc["expId"]);
    }

    sendOk(req, "config_applied");
}

// ─── POST /api/calibrate ─────────────────────────────────────────────────────
static void handleStartCalibration(AsyncWebServerRequest* req) {
    if (g_stateMachine.getState() != SystemState::IDLE) {
        sendError(req, "Must be IDLE to calibrate");
        return;
    }
    g_stateMachine.requestTransition(SystemState::CALIBRATION);
    sendOk(req, "calibration_started");
}

// ─── POST /api/confirm ───────────────────────────────────────────────────────
static void handleConfirmSample(AsyncWebServerRequest* req) {
    if (g_stateMachine.getState() != SystemState::WAIT_CONFIRMATION) {
        sendError(req, "Not in WAIT_CONFIRMATION state");
        return;
    }
    g_stateMachine.requestTransition(SystemState::MEASUREMENT);
    sendOk(req, "measurement_started");
}

// ─── POST /api/measure ───────────────────────────────────────────────────────
static void handleStartMeasure(AsyncWebServerRequest* req) {
    SystemState s = g_stateMachine.getState();
    if (s != SystemState::IDLE && s != SystemState::WAIT_CONFIRMATION) {
        sendError(req, "Cannot start measurement in current state");
        return;
    }
    g_stateMachine.requestTransition(SystemState::MEASUREMENT);
    sendOk(req, "measurement_started");
}

// ─── GET /api/spectra ────────────────────────────────────────────────────────
// Returns all acquired spectra for current experiment
static void handleGetSpectra(AsyncWebServerRequest* req) {
    const Experiment& exp = g_measurementEngine.getExperiment();

    // Build JSON: { expId, count, wavelengths:[...], spectra:[[...], ...] }
    // Using chunked response for large data
    String json = "{";
    json += "\"expId\":\"";  json += exp.experiment_id; json += "\",";
    json += "\"count\":";    json += exp.count;         json += ",";
    json += "\"wavelengths\":[";
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i) json += ",";
        json += AS7265xDriver::WAVELENGTHS[i];
    }
    json += "],\"spectra\":[";
    for (int m = 0; m < exp.count; m++) {
        if (m) json += ",";
        json += "[";
        for (int c = 0; c < NUM_CHANNELS; c++) {
            if (c) json += ",";
            json += String(exp.spectra[m][c], 4);
        }
        json += "]";
    }
    json += "]}";
    sendJson(req, json);
}

// ─── GET /api/calibration ────────────────────────────────────────────────────
static void handleGetCalibration(AsyncWebServerRequest* req) {
    const CalibrationData& cal = g_calibration.getData();
    String json = "{\"valid\":";
    json += cal.valid ? "true" : "false";
    json += ",\"offset\":[";
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i) json += ",";
        json += String(cal.offset[i], 4);
    }
    json += "]}";
    sendJson(req, json);
}

// ─── POST /api/save ──────────────────────────────────────────────────────────
static void handleSave(AsyncWebServerRequest* req) {
    if (g_stateMachine.getState() != SystemState::SAVE_DECISION) {
        sendError(req, "Not in SAVE_DECISION state");
        return;
    }
    bool ok = g_sdLogger.saveExperiment(g_measurementEngine.getExperiment());
    g_stateMachine.requestTransition(SystemState::IDLE);
    if (ok) sendOk(req, "saved");
    else    sendError(req, "SD write failed", 500);
}

// ─── POST /api/discard ───────────────────────────────────────────────────────
static void handleDiscard(AsyncWebServerRequest* req) {
    if (g_stateMachine.getState() != SystemState::SAVE_DECISION) {
        sendError(req, "Not in SAVE_DECISION state");
        return;
    }
    g_stateMachine.requestTransition(SystemState::IDLE);
    sendOk(req, "discarded");
}

// ─── POST /api/accept_validation ─────────────────────────────────────────────
static void handleAcceptValidation(AsyncWebServerRequest* req) {
    if (g_stateMachine.getState() != SystemState::VALIDATION) {
        sendError(req, "Not in VALIDATION state");
        return;
    }
    g_stateMachine.requestTransition(SystemState::SAVE_DECISION);
    sendOk(req, "proceed_to_save");
}

// ─── GET /api/wifi ────────────────────────────────────────────────────────────
static void handleGetWifi(AsyncWebServerRequest* req) {
    String status = wifiStaStatus();
    sendJson(req, "{\"status\":\"" + status + "\"}");
}

// ─── POST /api/wifi ───────────────────────────────────────────────────────────
// Body: { "ssid": "MyNetwork", "password": "secret" }
static void handleSetWifi(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
    StaticJsonDocument<192> doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) { sendError(req, "Invalid JSON"); return; }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["password"] | "";
    if (strlen(ssid) == 0) { sendError(req, "ssid required"); return; }
    wifiRequestConnect(ssid, pass);
    sendOk(req, "connecting");
}

// ─── Registration ─────────────────────────────────────────────────────────────
void registerApiRoutes(AsyncWebServer& server) {
    server.on("/api/status",     HTTP_GET,  handleGetStatus);
    server.on("/api/config",     HTTP_GET,  handleGetConfig);
    server.on("/api/spectra",    HTTP_GET,  handleGetSpectra);
    server.on("/api/calibration",HTTP_GET,  handleGetCalibration);
    server.on("/api/calibrate",  HTTP_POST, handleStartCalibration);
    server.on("/api/confirm",    HTTP_POST, handleConfirmSample);
    server.on("/api/measure",    HTTP_POST, handleStartMeasure);
    server.on("/api/accept",     HTTP_POST, handleAcceptValidation);
    server.on("/api/save",       HTTP_POST, handleSave);
    server.on("/api/discard",    HTTP_POST, handleDiscard);
    server.on("/api/wifi",       HTTP_GET,  handleGetWifi);

    // POST with body (config)
    server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req){},
        nullptr,
        handleSetConfig
    );

    // POST with body (wifi credentials)
    server.on("/api/wifi", HTTP_POST,
        [](AsyncWebServerRequest* req){},
        nullptr,
        handleSetWifi
    );
}

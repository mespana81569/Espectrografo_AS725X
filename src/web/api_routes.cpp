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
    StaticJsonDocument<384> doc;
    doc["gain"]             = (uint8_t)cfg.gain;
    doc["integrationCycles"] = cfg.integrationCycles;
    doc["mode"]             = (uint8_t)cfg.mode;
    doc["ledWhiteCurrent"]  = cfg.ledWhiteCurrent;
    doc["ledIrCurrent"]     = cfg.ledIrCurrent;
    doc["ledUvCurrent"]     = cfg.ledUvCurrent;
    doc["ledWhiteEnabled"]  = cfg.ledWhiteEnabled;
    doc["ledIrEnabled"]     = cfg.ledIrEnabled;
    doc["ledUvEnabled"]     = cfg.ledUvEnabled;
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
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) { sendError(req, "Invalid JSON"); return; }

    SensorConfig cfg = g_sensorDriver.getConfig();
    if (doc.containsKey("gain"))              cfg.gain             = (SensorGain)(uint8_t)doc["gain"];
    if (doc.containsKey("integrationCycles")) cfg.integrationCycles = doc["integrationCycles"];
    if (doc.containsKey("mode"))              cfg.mode             = (MeasurementMode)(uint8_t)doc["mode"];
    if (doc.containsKey("ledWhiteCurrent"))    cfg.ledWhiteCurrent  = doc["ledWhiteCurrent"];
    if (doc.containsKey("ledIrCurrent"))      cfg.ledIrCurrent     = doc["ledIrCurrent"];
    if (doc.containsKey("ledUvCurrent"))      cfg.ledUvCurrent     = doc["ledUvCurrent"];
    if (doc.containsKey("ledWhiteEnabled"))   cfg.ledWhiteEnabled  = doc["ledWhiteEnabled"];
    if (doc.containsKey("ledIrEnabled"))      cfg.ledIrEnabled     = doc["ledIrEnabled"];
    if (doc.containsKey("ledUvEnabled"))      cfg.ledUvEnabled     = doc["ledUvEnabled"];

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

// ─── Live monitor buffer (owned by main.cpp) ─────────────────────────────────
extern float    g_liveBuf[18];
extern volatile bool g_liveReady;

// ─── POST /api/monitor/start ─────────────────────────────────────────────────
static void handleMonitorStart(AsyncWebServerRequest* req) {
    if (g_stateMachine.getState() != SystemState::IDLE) {
        sendError(req, "Must be IDLE to start monitor");
        return;
    }
    g_stateMachine.requestTransition(SystemState::LIVE_MONITOR);
    sendOk(req, "monitor_started");
}

// ─── POST /api/monitor/stop ──────────────────────────────────────────────────
static void handleMonitorStop(AsyncWebServerRequest* req) {
    if (g_stateMachine.getState() != SystemState::LIVE_MONITOR) {
        sendError(req, "Not in LIVE_MONITOR state");
        return;
    }
    g_stateMachine.requestTransition(SystemState::IDLE);
    sendOk(req, "monitor_stopped");
}

// ─── GET /api/monitor ────────────────────────────────────────────────────────
static void handleMonitorData(AsyncWebServerRequest* req) {
    if (!g_liveReady) {
        sendJson(req, "{\"ready\":false}");
        return;
    }
    String json = "{\"ready\":true,\"ch\":[";
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i) json += ",";
        json += String(g_liveBuf[i], 4);
    }
    json += "],\"wl\":[";
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i) json += ",";
        json += AS7265xDriver::WAVELENGTHS[i];
    }
    json += "]}";
    sendJson(req, json);
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

// ─── POST /api/wifi/scan ─────────────────────────────────────────────────────
// Triggers a new WiFi scan (drops AP temporarily)
static void handleWifiScanStart(AsyncWebServerRequest* req) {
    if (wifiScanBusy()) {
        sendJson(req, "{\"scanning\":true}");
        return;
    }
    wifiStartScan();
    sendOk(req, "scan_started");
}

// ─── GET /api/wifi/scan ──────────────────────────────────────────────────────
// Returns cached scan results or scanning status
static void handleWifiScanGet(AsyncWebServerRequest* req) {
    if (wifiScanBusy()) {
        sendJson(req, "{\"scanning\":true,\"networks\":[]}");
        return;
    }
    sendJson(req, wifiScanResultsJson());
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
    server.on("/api/wifi/scan",  HTTP_GET,  handleWifiScanGet);
    server.on("/api/wifi/scan",  HTTP_POST, handleWifiScanStart);
    server.on("/api/monitor",    HTTP_GET,  handleMonitorData);
    server.on("/api/monitor/start", HTTP_POST, handleMonitorStart);
    server.on("/api/monitor/stop",  HTTP_POST, handleMonitorStop);

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

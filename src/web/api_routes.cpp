#include "api_routes.h"
#include "web_server.h"
#include "../core/state_machine.h"
#include "../sensors/as7265x_driver.h"
#include "../acquisition/measurement_engine.h"
#include "../acquisition/calibration.h"
#include "../storage/sd_logger.h"
#include "../mqtt/mqtt_client.h"
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
    StaticJsonDocument<512> doc;
    const Experiment& exp = g_measurementEngine.getExperiment();
    const CalibrationData& cal = g_calibration.getData();
    doc["state"]       = g_stateMachine.getStateName();
    doc["sensorReady"] = g_sensorDriver.isReady();
    doc["sdReady"]     = g_sdLogger.isReady();
    doc["calValid"]    = cal.valid;
    doc["calN"]        = cal.n_used;
    // Live progress for both phases — drives the dashboard progress bars.
    doc["calProgress"]    = g_calibration.samplesCollected();
    doc["calTarget"]      = g_calibration.samplesTarget();
    doc["measCount"]   = exp.count;
    doc["measTarget"]  = exp.num_measurements;
    doc["uuid"]        = exp.uuid;
    doc["expId"]       = exp.experiment_id;
    // Surface the calibration's snapshotted gain/integration so the dashboard
    // can show a tooltip "calibration invalid because gain changed from 16x
    // to 64x" rather than a silent red dot.
    JsonObject c2 = doc.createNestedObject("calCfg");
    c2["gain"]       = (uint8_t)cal.cfg_at_cal.gain;
    c2["int_cycles"] = cal.cfg_at_cal.integrationCycles;
    JsonObject c1 = doc.createNestedObject("liveCfg");
    c1["gain"]       = (uint8_t)g_sensorDriver.getConfig().gain;
    c1["int_cycles"] = g_sensorDriver.getConfig().integrationCycles;
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
    doc["nCal"]             = cfg.nCal;
    doc["nCalUseSameAsN"]   = cfg.nCalUseSameAsN;
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

    SensorConfig prev = g_sensorDriver.getConfig();
    SensorConfig cfg  = prev;
    if (doc.containsKey("gain"))              cfg.gain             = (SensorGain)(uint8_t)doc["gain"];
    if (doc.containsKey("integrationCycles")) cfg.integrationCycles = doc["integrationCycles"];
    if (doc.containsKey("mode"))              cfg.mode             = (MeasurementMode)(uint8_t)doc["mode"];
    if (doc.containsKey("ledWhiteCurrent"))   cfg.ledWhiteCurrent  = doc["ledWhiteCurrent"];
    if (doc.containsKey("ledIrCurrent"))      cfg.ledIrCurrent     = doc["ledIrCurrent"];
    if (doc.containsKey("ledUvCurrent"))      cfg.ledUvCurrent     = doc["ledUvCurrent"];
    if (doc.containsKey("ledWhiteEnabled"))   cfg.ledWhiteEnabled  = doc["ledWhiteEnabled"];
    if (doc.containsKey("ledIrEnabled"))      cfg.ledIrEnabled     = doc["ledIrEnabled"];
    if (doc.containsKey("ledUvEnabled"))      cfg.ledUvEnabled     = doc["ledUvEnabled"];
    if (doc.containsKey("nCal"))              cfg.nCal             = (uint8_t)doc["nCal"];
    if (doc.containsKey("nCalUseSameAsN"))    cfg.nCalUseSameAsN   = doc["nCalUseSameAsN"];

    // Clarification #5: counts only ratio-meaningfully across identical
    // gain/integration/LED state.  If the user changed any of those, the
    // existing blank reference is no longer a valid divisor — invalidate it
    // here so subsequent measurements know to refuse transmittance until
    // the user re-runs calibration.  The applied state is reflected in
    // /api/status (calValid=false) and in the heartbeat so both UIs see it.
    if (!sensorConfigCountsComparable(prev, cfg) && g_calibration.getData().valid) {
        g_calibration.reset();
        Serial.println("[Cfg] sensor config changed — calibration invalidated");
    }

    g_sensorDriver.applyConfig(cfg);

    int nReq = doc.containsKey("N") ? (int)doc["N"] : 5;
    int n = nReq;
    String warning = "";
    if (n > MAX_MEASUREMENTS) {
        warning = "N=" + String(nReq) + " exceeds MAX_MEASUREMENTS (" +
                  String(MAX_MEASUREMENTS) + "); clamped to " + String(MAX_MEASUREMENTS);
        n = MAX_MEASUREMENTS;
    } else if (n < 1) {
        warning = "N=" + String(nReq) + " below minimum (1); clamped to 1";
        n = 1;
    }
    g_measurementEngine.configure(n, cfg);

    if (doc.containsKey("expId")) {
        g_measurementEngine.resetExperiment(doc["expId"]);
    }

    StaticJsonDocument<256> resp;
    resp["status"] = "config_applied";
    resp["N"] = n;
    if (warning.length() > 0) resp["warning"] = warning;
    String out;
    serializeJson(resp, out);
    sendJson(req, out);
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

// Helper: emit a JSON array of NUM_CHANNELS floats.  NaN is emitted as
// the JSON literal `null` (the chart code understands "no value here").
// Previously NaN was emitted as 0.0, which silently made absorbing channels
// look transparent — masking exactly the bug we want to surface.
static void appendChannelArray(String& json, const float* row) {
    json += "[";
    for (int c = 0; c < NUM_CHANNELS; c++) {
        if (c) json += ",";
        float v = row[c];
        if (!isfinite(v)) json += "null";
        else              json += String(v, 4);
    }
    json += "]";
}

// ─── GET /api/spectra ────────────────────────────────────────────────────────
// Returns the CURRENT experiment's raw Δ rows + the per-experiment calibration
// (the snapshot taken at MeasurementEngine::start, NOT the live calibration).
// This is the contract the chart depends on for correct transmittance: the I0
// it divides by must be the I0 that produced the Δ.
static void handleGetSpectra(AsyncWebServerRequest* req) {
    const Experiment& exp = g_measurementEngine.getExperiment();

    int safeCount = exp.count;
    if (safeCount < 0) safeCount = 0;
    if (safeCount > MAX_MEASUREMENTS) safeCount = MAX_MEASUREMENTS;

    String json = "{";
    json += "\"uuid\":\"";   json += exp.uuid;          json += "\",";
    json += "\"expId\":\"";  json += exp.experiment_id; json += "\",";
    json += "\"count\":";    json += safeCount;         json += ",";
    json += "\"processed\":"; json += exp.processed ? "true" : "false"; json += ",";
    json += "\"wavelengths\":[";
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i) json += ",";
        json += AS7265xDriver::WAVELENGTHS[i];
    }
    json += "],\"spectra\":[";
    for (int m = 0; m < safeCount; m++) {
        if (m) json += ",";
        appendChannelArray(json, exp.spectra[m]);
    }
    json += "],\"calValid\":";
    json += exp.calibration.valid ? "true" : "false";
    json += ",\"offsets\":";
    appendChannelArray(json, exp.calibration.offset);
    json += "}";
    sendJson(req, json);
}

// ─── GET /api/transmittance & /api/absorbance ───────────────────────────────
// Both follow the same shape — only the field name changes — so we factor.
static void handleGetProcessed(AsyncWebServerRequest* req, bool absorbance) {
    const Experiment& exp = g_measurementEngine.getExperiment();
    int safeCount = exp.count;
    if (safeCount < 0) safeCount = 0;
    if (safeCount > MAX_MEASUREMENTS) safeCount = MAX_MEASUREMENTS;

    String json = "{";
    json += "\"uuid\":\"";   json += exp.uuid;          json += "\",";
    json += "\"expId\":\"";  json += exp.experiment_id; json += "\",";
    json += "\"count\":";    json += safeCount;         json += ",";
    json += "\"processed\":"; json += exp.processed ? "true" : "false"; json += ",";
    json += "\"calValid\":"; json += exp.calibration.valid ? "true" : "false"; json += ",";
    json += "\"unit\":\""; json += absorbance ? "a.u." : "%"; json += "\",";
    json += "\"wavelengths\":[";
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i) json += ",";
        json += AS7265xDriver::WAVELENGTHS[i];
    }
    json += "],\"rows\":[";
    for (int m = 0; m < safeCount; m++) {
        if (m) json += ",";
        appendChannelArray(json, absorbance ? exp.absorbance[m] : exp.transmittance[m]);
    }
    json += "]}";
    sendJson(req, json);
}
static void handleGetTransmittance(AsyncWebServerRequest* req) { handleGetProcessed(req, false); }
static void handleGetAbsorbance   (AsyncWebServerRequest* req) { handleGetProcessed(req, true);  }

// ─── GET /api/calibration ────────────────────────────────────────────────────
static void handleGetCalibration(AsyncWebServerRequest* req) {
    const CalibrationData& cal = g_calibration.getData();
    String json = "{\"valid\":";
    json += cal.valid ? "true" : "false";
    json += ",\"offset\":[";
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i) json += ",";
        float v = cal.offset[i];
        if (!isfinite(v)) v = 0.0f;
        json += String(v, 4);
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
        float v = g_liveBuf[i];
        if (!isfinite(v)) v = 0.0f;
        json += String(v, 4);
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
// Triggers a scan from loop() — AP will drop, phone will disconnect briefly
static void handleWifiScanStart(AsyncWebServerRequest* req) {
    if (wifiScanInProgress()) {
        sendJson(req, "{\"status\":\"already_scanning\"}");
        return;
    }
    wifiRequestScan();
    sendOk(req, "scan_started");
}

// ─── GET /api/wifi/scan ──────────────────────────────────────────────────────
// Returns scan state + cached results
static void handleWifiScanGet(AsyncWebServerRequest* req) {
    if (wifiScanInProgress()) {
        sendJson(req, "{\"scanning\":true}");
        return;
    }
    if (wifiScanHasResults()) {
        sendJson(req, wifiScanResultsJson());
        return;
    }
    sendJson(req, "{\"scanning\":false,\"networks\":[]}");
}

// ─── Registration ─────────────────────────────────────────────────────────────
void registerApiRoutes(AsyncWebServer& server) {
    server.on("/api/status",     HTTP_GET,  handleGetStatus);
    server.on("/api/config",     HTTP_GET,  handleGetConfig);
    server.on("/api/spectra",       HTTP_GET, handleGetSpectra);
    server.on("/api/transmittance", HTTP_GET, handleGetTransmittance);
    server.on("/api/absorbance",    HTTP_GET, handleGetAbsorbance);
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

#include "mqtt_client.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <math.h>
#include <string.h>

#include "../core/state_machine.h"
#include "../sensors/as7265x_driver.h"
#include "../acquisition/calibration.h"
#include "../storage/sd_logger.h"

MqttClient  g_mqttClient;
MqttClient* MqttClient::_instance = nullptr;

// CSV row peek-ahead — when the upload state machine reads a row whose exp_id
// does not match the currently-buffered group, we stash that row here and
// process it as the first row of the next group on the following tick.
static File   s_uploadFile;
static bool   s_uploadFileOpen = false;
static String s_lookaheadLine;
static bool   s_lookaheadValid = false;

// ─── Small helpers ──────────────────────────────────────────────────────────

// Destructive in-place CSV splitter. Returns number of fields.
static int splitCsv(char* line, char** fields, int maxFields) {
    int n = 0;
    if (!line || !*line) return 0;
    fields[n++] = line;
    for (char* p = line; *p && n < maxFields; p++) {
        if (*p == ',') {
            *p = 0;
            fields[n++] = p + 1;
        }
    }
    // Trim trailing \r on final field
    if (n > 0) {
        char* last = fields[n - 1];
        size_t L = strlen(last);
        while (L > 0 && (last[L - 1] == '\r' || last[L - 1] == '\n')) {
            last[--L] = 0;
        }
    }
    return n;
}

static int gainStrToInt(const char* s) {
    if (!s) return 2;
    if (!strcmp(s, "1x"))  return 0;
    if (!strcmp(s, "4x"))  return 1;
    if (!strcmp(s, "16x")) return 2;
    if (!strcmp(s, "64x")) return 3;
    return 2;
}

static const uint16_t WL18[NUM_CHANNELS] = {
    410, 435, 460, 485, 510, 535,
    560, 585, 610, 645, 680, 705,
    730, 760, 810, 860, 900, 940
};

static void appendWavelengths(String& out) {
    out += "[";
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i) out += ",";
        out += WL18[i];
    }
    out += "]";
}

static void appendFloatArray(String& out, const float* v, int n, int decimals = 4) {
    out += "[";
    for (int i = 0; i < n; i++) {
        if (i) out += ",";
        // NaN/Inf as JSON `null` — chart code treats null as "no value".
        // Previously we emitted "nan" which broke the JS JSON.parse silently.
        if (!isfinite(v[i])) out += "null";
        else                 out += String(v[i], decimals);
    }
    out += "]";
}

// ─── Construction / lifecycle ───────────────────────────────────────────────

MqttClient::MqttClient()
    : _client(_wifiClient),
      _lastReconnectAttempt(0),
      _lastHeartbeat(0),
      _lastLiveFrame(0),
      _lastCalK(-1),
      _lastMeasK(-1),
      _groupRetries(0),
      _uploadedOk(0),
      _uploadedFail(0),
      _wasConnected(false),
      _pendingCalibrate(false),
      _pendingConfirm(false),
      _pendingAccept(false),
      _pendingSave(false),
      _pendingDiscard(false),
      _pendingConfig(false),
      _pendingConfigLen(0),
      _pendingPullData(false),
      _pendingMonitorStart(false),
      _pendingMonitorStop(false),
      _uploadState(UploadState::IDLE),
      _uploadSucceeded(false) {
    memset(_pendingConfigBuf, 0, sizeof(_pendingConfigBuf));
    memset(&_group, 0, sizeof(_group));
}

void MqttClient::begin() {
    _instance = this;
    _client.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    _client.setCallback(&MqttClient::staticCallback);
    _client.setBufferSize(MQTT_MAX_PACKET_SIZE);
    _client.setKeepAlive(30);
    _client.setSocketTimeout(5);
    Serial.printf("[MQTT] Configured broker %s:%u\n",
                  MQTT_BROKER_HOST, (unsigned)MQTT_BROKER_PORT);
}

bool MqttClient::isConnected() {
    return _client.connected();
}

// ─── Publish ────────────────────────────────────────────────────────────────

bool MqttClient::publish(const char* topic, const char* payload, bool retained) {
    if (!_client.connected()) return false;
    return _client.publish(topic, payload, retained);
}

bool MqttClient::publish(const char* topic, const char* payload,
                         uint8_t qos, bool retained) {
    // PubSubClient's QoS-aware overload: beginPublish/write/endPublish do not
    // support QoS>0 in this library (only MQTT 3.1.1 QoS 0/1 publish without
    // ACK handling for the user). We use the standard publish() which the
    // broker treats as QoS 0 in PubSubClient. QoS parameter retained for API
    // compatibility — the library limitation is documented upstream.
    (void)qos;
    return publish(topic, payload, retained);
}

void MqttClient::publishState(const char* stateName) {
    if (!stateName) return;
    if (!_client.connected()) return;
    _client.publish(MQTT_TOPIC_DATA_STATE, stateName, false);
}

void MqttClient::publishExperiment(const Experiment& exp) {
    if (!_client.connected()) return;
    String json = buildExperimentJson(exp);
    bool ok = _client.publish(MQTT_TOPIC_DATA_SPECTRA, json.c_str(), false);
    Serial.printf("[MQTT] publish /data/spectra (%u bytes) %s\n",
                  (unsigned)json.length(), ok ? "ok" : "FAIL");
}

// ─── Tick ───────────────────────────────────────────────────────────────────

void MqttClient::tick() {
    // 1) Only attempt broker I/O while the STA link is up.
    if (WiFi.status() != WL_CONNECTED) {
        if (_wasConnected) {
            Serial.println("[MQTT] WiFi dropped — will reconnect when link returns");
            _wasConnected = false;
        }
        return;
    }

    // 2) Reconnect with 5s backoff — never block.
    if (!_client.connected()) {
        if (_wasConnected) {
            Serial.println("[MQTT] Disconnected from broker");
            _wasConnected = false;
            // Any in-flight upload must restart after a clean reconnect.
            _uploadState = UploadState::IDLE;
            if (s_uploadFileOpen) { s_uploadFile.close(); s_uploadFileOpen = false; }
            s_lookaheadValid = false;
        }
        unsigned long now = millis();
        if (now - _lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
            _lastReconnectAttempt = now;
            attemptConnect();
        }
        return;
    }

    // 3) Pump the protocol (no delay() inside — well-behaved).
    _client.loop();

    // 4) Edge: first successful connect → subscribe only. Bulk upload is
    //    now user-triggered via esp32/cmd/pull_data.
    if (!_wasConnected) {
        _wasConnected = true;
        Serial.println("[MQTT] Connected to broker");
        subscribeAll();
        publishState(g_stateMachine.getStateName());
    }

    // 5) Apply any deferred commands captured from the callback.
    processPendingCommands();

    // 6) Drive bulk upload at most one "unit" per tick.
    if (_uploadState != UploadState::IDLE) {
        processUploadTick();
    }

    // 7) Periodic heartbeat.
    unsigned long now = millis();
    if (now - _lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        _lastHeartbeat = now;
        publishHeartbeat();
    }

    // 8) Live frames — drives the dashboard's progress bars and plots.
    //    Throttled to 500 ms (clarification #3); each phase is a no-op
    //    when the corresponding state machine state isn't active.
    if (now - _lastLiveFrame >= LIVE_INTERVAL_MS) {
        _lastLiveFrame = now;
        publishLiveFrames();
    }
}

bool MqttClient::attemptConnect() {
    Serial.printf("[MQTT] Connecting to %s:%u as %s...\n",
                  MQTT_BROKER_HOST, (unsigned)MQTT_BROKER_PORT, MQTT_CLIENT_ID);
    bool ok = _client.connect(MQTT_CLIENT_ID);
    if (!ok) {
        Serial.printf("[MQTT] connect() failed, rc=%d\n", _client.state());
    }
    return ok;
}

void MqttClient::subscribeAll() {
    _client.subscribe(MQTT_TOPIC_CMD_CALIBRATE);
    _client.subscribe(MQTT_TOPIC_CMD_CONFIRM);
    _client.subscribe(MQTT_TOPIC_CMD_ACCEPT);
    _client.subscribe(MQTT_TOPIC_CMD_SAVE);
    _client.subscribe(MQTT_TOPIC_CMD_DISCARD);
    _client.subscribe(MQTT_TOPIC_CMD_CONFIG);
    _client.subscribe(MQTT_TOPIC_CMD_PULL_DATA);
    _client.subscribe(MQTT_TOPIC_CMD_MONITOR_START);
    _client.subscribe(MQTT_TOPIC_CMD_MONITOR_STOP);
    Serial.println("[MQTT] Subscribed to esp32/cmd/*");
}

void MqttClient::triggerSDUpload() {
    // Issue #9 — pull failures must be LOUD.  Previously this returned
    // silently and the dashboard's "Pull from ESP32" button looked like it
    // had succeeded.  Now every refusal publishes to the error topic so
    // control.html can surface the message in its banner.
    if (!g_sdLogger.isReady()) {
        Serial.println("[MQTT] SD not ready — refusing bulk upload");
        _client.publish(MQTT_TOPIC_DATA_PULL_ERROR,
                        "{\"requested\":\"all\",\"reason\":\"sd_not_ready\"}", false);
        return;
    }
    if (!SD.exists(LOG_FILE)) {
        Serial.println("[MQTT] No /spectra.csv to upload");
        _client.publish(MQTT_TOPIC_DATA_PULL_ERROR,
                        "{\"requested\":\"all\",\"reason\":\"no_data\"}", false);
        return;
    }
    _uploadState = UploadState::OPENING;
    _uploadSucceeded = false;
    _groupRetries  = 0;
    _uploadedOk    = 0;
    _uploadedFail  = 0;
    memset(&_group, 0, sizeof(_group));
    s_lookaheadValid = false;
    Serial.println("[MQTT] Bulk upload scheduled");
}

// ─── Callback (runs on Core 1 inside _client.loop()) ────────────────────────

void MqttClient::staticCallback(char* topic, uint8_t* payload, unsigned int len) {
    if (_instance) _instance->handleCallback(topic, payload, len);
}

void MqttClient::handleCallback(const char* topic, const uint8_t* payload, unsigned int len) {
    // Rule: never touch sensor/SD/state hardware directly — only raise flags
    // or copy buffers. The real work happens in tick() → processPendingCommands.
    if (!strcmp(topic, MQTT_TOPIC_CMD_CALIBRATE)) {
        _pendingCalibrate = true;
    } else if (!strcmp(topic, MQTT_TOPIC_CMD_CONFIRM)) {
        _pendingConfirm = true;
    } else if (!strcmp(topic, MQTT_TOPIC_CMD_ACCEPT)) {
        _pendingAccept = true;
    } else if (!strcmp(topic, MQTT_TOPIC_CMD_SAVE)) {
        _pendingSave = true;
    } else if (!strcmp(topic, MQTT_TOPIC_CMD_DISCARD)) {
        _pendingDiscard = true;
    } else if (!strcmp(topic, MQTT_TOPIC_CMD_CONFIG)) {
        size_t n = len;
        if (n >= sizeof(_pendingConfigBuf)) n = sizeof(_pendingConfigBuf) - 1;
        memcpy(_pendingConfigBuf, payload, n);
        _pendingConfigBuf[n] = 0;
        _pendingConfigLen = n;
        _pendingConfig = true;
    } else if (!strcmp(topic, MQTT_TOPIC_CMD_PULL_DATA)) {
        _pendingPullData = true;
    } else if (!strcmp(topic, MQTT_TOPIC_CMD_MONITOR_START)) {
        _pendingMonitorStart = true;
    } else if (!strcmp(topic, MQTT_TOPIC_CMD_MONITOR_STOP)) {
        _pendingMonitorStop = true;
    }
}

// ─── Deferred command execution ─────────────────────────────────────────────

void MqttClient::processPendingCommands() {
    SystemState s = g_stateMachine.getState();

    if (_pendingCalibrate) {
        _pendingCalibrate = false;
        if (s == SystemState::IDLE) {
            g_stateMachine.requestTransition(SystemState::CALIBRATION);
        } else {
            Serial.printf("[MQTT] calibrate ignored — state %s\n",
                          g_stateMachine.getStateName());
        }
    }

    if (_pendingConfirm) {
        _pendingConfirm = false;
        if (g_stateMachine.getState() == SystemState::WAIT_CONFIRMATION) {
            g_stateMachine.requestTransition(SystemState::MEASUREMENT);
        }
    }

    if (_pendingAccept) {
        _pendingAccept = false;
        if (g_stateMachine.getState() == SystemState::VALIDATION) {
            g_stateMachine.requestTransition(SystemState::SAVE_DECISION);
        }
    }

    if (_pendingSave) {
        _pendingSave = false;
        if (g_stateMachine.getState() == SystemState::SAVE_DECISION) {
            // Mirror the REST /api/save flow: persist locally only. Upload to
            // the DB is deferred until the user presses Pull Data.
            (void)g_sdLogger.saveExperiment(g_measurementEngine.getExperiment());
            g_stateMachine.requestTransition(SystemState::IDLE);
        }
    }

    if (_pendingDiscard) {
        _pendingDiscard = false;
        if (g_stateMachine.getState() == SystemState::SAVE_DECISION) {
            g_stateMachine.requestTransition(SystemState::IDLE);
        }
    }

    if (_pendingPullData) {
        _pendingPullData = false;
        if (_uploadState == UploadState::IDLE) {
            Serial.println("[MQTT] pull_data — triggering SD upload");
            triggerSDUpload();
        } else {
            Serial.println("[MQTT] pull_data ignored — upload already in progress");
        }
    }

    if (_pendingMonitorStart) {
        _pendingMonitorStart = false;
        // Mirrors handleMonitorStart in api_routes.cpp: only enter live
        // monitor from IDLE.  Without this gate the dashboard could yank
        // the device out of an in-progress measurement.
        if (g_stateMachine.getState() == SystemState::IDLE) {
            g_stateMachine.requestTransition(SystemState::LIVE_MONITOR);
        } else {
            Serial.printf("[MQTT] monitor/start ignored — state %s\n",
                          g_stateMachine.getStateName());
        }
    }

    if (_pendingMonitorStop) {
        _pendingMonitorStop = false;
        if (g_stateMachine.getState() == SystemState::LIVE_MONITOR) {
            g_stateMachine.requestTransition(SystemState::IDLE);
        }
    }

    if (_pendingConfig) {
        _pendingConfig = false;
        if (g_stateMachine.getState() != SystemState::IDLE) {
            Serial.println("[MQTT] config ignored — not IDLE");
        } else {
            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, _pendingConfigBuf, _pendingConfigLen);
            if (err) {
                Serial.printf("[MQTT] config parse error: %s\n", err.c_str());
            } else {
                SensorConfig prev = g_sensorDriver.getConfig();
                SensorConfig cfg  = prev;
                if (doc.containsKey("gain"))              cfg.gain              = (SensorGain)(uint8_t)doc["gain"];
                if (doc.containsKey("integrationCycles")) cfg.integrationCycles = doc["integrationCycles"];
                if (doc.containsKey("mode"))              cfg.mode              = (MeasurementMode)(uint8_t)doc["mode"];
                if (doc.containsKey("ledWhiteCurrent"))   cfg.ledWhiteCurrent   = doc["ledWhiteCurrent"];
                if (doc.containsKey("ledIrCurrent"))      cfg.ledIrCurrent      = doc["ledIrCurrent"];
                if (doc.containsKey("ledUvCurrent"))      cfg.ledUvCurrent      = doc["ledUvCurrent"];
                if (doc.containsKey("ledWhiteEnabled"))   cfg.ledWhiteEnabled   = doc["ledWhiteEnabled"];
                if (doc.containsKey("ledIrEnabled"))      cfg.ledIrEnabled      = doc["ledIrEnabled"];
                if (doc.containsKey("ledUvEnabled"))      cfg.ledUvEnabled      = doc["ledUvEnabled"];
                if (doc.containsKey("nCal"))              cfg.nCal              = (uint8_t)doc["nCal"];
                if (doc.containsKey("nCalUseSameAsN"))    cfg.nCalUseSameAsN    = doc["nCalUseSameAsN"];

                // Same invariant as the REST handler — see api_routes.cpp.
                if (!sensorConfigCountsComparable(prev, cfg) && g_calibration.getData().valid) {
                    g_calibration.reset();
                    Serial.println("[MQTT] sensor config changed — calibration invalidated");
                }

                g_sensorDriver.applyConfig(cfg);

                if (doc.containsKey("N")) {
                    g_measurementEngine.configure((int)doc["N"], cfg);
                }
                if (doc.containsKey("expId")) {
                    g_measurementEngine.resetExperiment(doc["expId"]);
                }
                Serial.println("[MQTT] config applied");
            }
        }
    }
}

// ─── Live frames (issue #7 — feeds dashboard progress + live plot) ──────────
//
// All three publishers share the same throttling tick.  Payloads are kept
// small (<256 B) so a 500 ms cadence is comfortable even over flaky links.
// We also short-circuit a publish when the relevant counter hasn't moved —
// no point spending broker bytes to confirm "still 3/5".

extern float    g_liveBuf[18];
extern volatile bool g_liveReady;

void MqttClient::publishLiveFrames() {
    if (!_client.connected()) return;
    SystemState st = g_stateMachine.getState();

    if (st == SystemState::CALIBRATION) {
        int k = g_calibration.samplesCollected();
        int n = g_calibration.samplesTarget();
        if (k != _lastCalK) {
            _lastCalK = k;
            char buf[96];
            int len = snprintf(buf, sizeof(buf),
                               "{\"phase\":\"calibration\",\"k\":%d,\"n\":%d}", k, n);
            _client.publish(MQTT_TOPIC_DATA_CAL_PROGRESS,
                            (const uint8_t*)buf, len, false);
        }
    } else {
        _lastCalK = -1;
    }

    if (st == SystemState::MEASUREMENT) {
        const Experiment& exp = g_measurementEngine.getExperiment();
        int k = exp.count;
        int n = exp.num_measurements;
        if (k != _lastMeasK && k > 0) {
            _lastMeasK = k;
            // Include the most recent row so the dashboard's "last
            // measurements" plot can update incrementally — without this,
            // users only see the experiment after it's fully uploaded.
            String out;
            out.reserve(384);
            out += "{\"phase\":\"measurement\",\"uuid\":\"";
            out += exp.uuid;
            out += "\",\"exp_id\":\""; out += exp.experiment_id;
            out += "\",\"k\":"; out += k; out += ",\"n\":"; out += n;
            out += ",\"row\":";
            appendFloatArray(out, exp.spectra[k - 1], NUM_CHANNELS);
            out += "}";
            _client.publish(MQTT_TOPIC_DATA_MEAS_PROGRESS, out.c_str(), false);
        }
    } else {
        _lastMeasK = -1;
    }

    if (st == SystemState::LIVE_MONITOR && g_liveReady) {
        String out;
        out.reserve(384);
        out += "{\"ch\":";
        appendFloatArray(out, g_liveBuf, NUM_CHANNELS, 1);
        out += ",\"wl\":";
        appendWavelengths(out);
        out += "}";
        _client.publish(MQTT_TOPIC_DATA_MONITOR, out.c_str(), false);
    }
}

// ─── Heartbeat ──────────────────────────────────────────────────────────────

void MqttClient::publishHeartbeat() {
    const Experiment& exp = g_measurementEngine.getExperiment();
    const CalibrationData& cal = g_calibration.getData();
    StaticJsonDocument<384> doc;
    doc["state"]      = g_stateMachine.getStateName();
    doc["rssi"]       = WiFi.RSSI();
    doc["heap"]       = (int)ESP.getFreeHeap();
    doc["sd_ready"]   = g_sdLogger.isReady();
    doc["uuid"]       = exp.uuid;
    doc["exp_id"]     = exp.experiment_id;
    doc["calValid"]   = cal.valid;
    doc["calProgress"] = g_calibration.samplesCollected();
    doc["calTarget"]   = g_calibration.samplesTarget();
    doc["measCount"]  = exp.count;
    doc["measTarget"] = exp.num_measurements;
    char buf[384];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    _client.publish(MQTT_TOPIC_DATA_STATUS, (const uint8_t*)buf, n, false);
}

// ─── JSON builders ──────────────────────────────────────────────────────────

String MqttClient::buildExperimentJson(const Experiment& exp) {
    String out;
    out.reserve(4096);
    out += "{\"uuid\":\"";          out += exp.uuid;
    out += "\",\"exp_id\":\"";      out += exp.experiment_id;
    out += "\",\"timestamp_ms\":";  out += (unsigned long)exp.timestamp;
    out += ",\"num_measurements\":"; out += exp.num_measurements;
    out += ",\"n_cal\":";           out += (int)exp.calibration.n_used;
    out += ",\"sensor\":{\"gain\":"; out += (int)(uint8_t)exp.sensor_cfg.gain;
    out += ",\"mode\":";            out += (int)(uint8_t)exp.sensor_cfg.mode;
    out += ",\"int_cycles\":";      out += (int)exp.sensor_cfg.integrationCycles;
    out += ",\"led_white_ma\":";    out += (int)exp.sensor_cfg.ledWhiteCurrent;
    out += ",\"led_ir_ma\":";       out += (int)exp.sensor_cfg.ledIrCurrent;
    out += ",\"led_uv_ma\":";       out += (int)exp.sensor_cfg.ledUvCurrent;
    out += ",\"led_white_on\":";    out += exp.sensor_cfg.ledWhiteEnabled ? "true" : "false";
    out += ",\"led_ir_on\":";       out += exp.sensor_cfg.ledIrEnabled    ? "true" : "false";
    out += ",\"led_uv_on\":";       out += exp.sensor_cfg.ledUvEnabled    ? "true" : "false";
    out += "},\"calibration\":{\"valid\":";
    out += exp.calibration.valid ? "true" : "false";
    out += ",\"offsets\":";
    appendFloatArray(out, exp.calibration.offset, NUM_CHANNELS);
    out += ",\"cfg_at_cal\":{\"gain\":";
    out += (int)(uint8_t)exp.calibration.cfg_at_cal.gain;
    out += ",\"int_cycles\":";
    out += (int)exp.calibration.cfg_at_cal.integrationCycles;
    out += "}},\"spectra\":[";
    for (int i = 0; i < exp.count; i++) {
        if (i) out += ",";
        appendFloatArray(out, exp.spectra[i], NUM_CHANNELS);
    }
    // Processed values from MeasurementEngine::computeProcessed().
    out += "],\"transmittance\":[";
    for (int i = 0; i < exp.count; i++) {
        if (i) out += ",";
        appendFloatArray(out, exp.transmittance[i], NUM_CHANNELS, 3);
    }
    out += "],\"absorbance\":[";
    for (int i = 0; i < exp.count; i++) {
        if (i) out += ",";
        appendFloatArray(out, exp.absorbance[i], NUM_CHANNELS, 4);
    }
    out += "],\"wavelengths_nm\":";
    appendWavelengths(out);
    out += "}";
    return out;
}

// Group payload reconstructed from CSV during the SD bulk-upload pass.
//
// IMPORTANT: this payload contains ONLY raw Δ + offsets — transmittance and
// absorbance are NOT included.  Reasoning:
//   * Each (T row + A row) doubles the byte count vs. raw alone.  A 20-
//     measurement experiment with raw+T+A pushed past the historical 4 KB
//     buffer and any oversized publish silently fails (PubSubClient returns
//     false; the previous code "logged FAIL" and moved on, dropping the
//     entire experiment).
//   * The bridge (server/mqtt_to_db.py) recomputes T+A from raw+offsets on
//     ingest — same math, no data dependence.  See store_experiment().
// Live-side spectra publishes (publishExperiment) still include T+A because
// they're produced fresh on-device and the dashboard wants to render them
// without waiting for the bridge round-trip.
String MqttClient::buildGroupJson() const {
    String out;
    out.reserve(8192);
    out += "{\"uuid\":\"";          out += _group.uuid;
    out += "\",\"exp_id\":\"";      out += _group.exp_id;
    out += "\",\"timestamp_ms\":0,\"num_measurements\":"; out += _group.count;
    out += ",\"n_cal\":0";  // not preserved in the CSV layout — best-effort
    out += ",\"sensor\":{\"gain\":";  out += _group.gain_int;
    out += ",\"mode\":";              out += _group.mode;
    out += ",\"int_cycles\":";        out += _group.int_cycles;
    out += ",\"led_white_ma\":";      out += _group.led_white_ma;
    out += ",\"led_ir_ma\":";         out += _group.led_ir_ma;
    out += ",\"led_uv_ma\":";         out += _group.led_uv_ma;
    out += ",\"led_white_on\":";      out += _group.led_white_en ? "true" : "false";
    out += ",\"led_ir_on\":";         out += _group.led_ir_en    ? "true" : "false";
    out += ",\"led_uv_on\":";         out += _group.led_uv_en    ? "true" : "false";
    out += "},\"calibration\":{\"valid\":";
    out += _group.cal_valid ? "true" : "false";
    out += ",\"offsets\":";
    appendFloatArray(out, _group.offsets, NUM_CHANNELS);
    out += ",\"cfg_at_cal\":{\"gain\":"; out += _group.gain_int;
    out += ",\"int_cycles\":";           out += _group.int_cycles;
    out += "}},\"spectra\":[";
    for (int i = 0; i < _group.count; i++) {
        if (i) out += ",";
        appendFloatArray(out, _group.spectra[i], NUM_CHANNELS);
    }
    // T+A intentionally OMITTED here (see header docblock above) — bridge
    // recomputes them from raw + offsets so the bulk-upload payload stays
    // small enough to never overflow the MQTT buffer.
    out += "],\"wavelengths_nm\":";
    appendWavelengths(out);
    out += "}";
    return out;
}

// ─── SD bulk upload state machine ───────────────────────────────────────────
//
// State progression driven one step per tick():
//   IDLE     → OPENING    (triggerSDUpload sets this)
//   OPENING  → READING    (file handle acquired, header line consumed)
//   READING  → READING    (publish one experiment group per pass)
//            → FINISHING  (EOF reached, group buffer drained)
//   FINISHING → IDLE      (file removed, UPLOAD_COMPLETE published)

static int parseHeaderRow(MqttClient::UploadGroup* /*dummy — layout only*/) { return 0; }

// Helpers that need access to private members are implemented as methods.
void MqttClient::processUploadTick() {
    if (_uploadState == UploadState::OPENING) {
        if (uploadOpenFile()) {
            _uploadState = UploadState::READING;
        } else {
            Serial.println("[MQTT] Upload: failed to open file");
            _client.publish(MQTT_TOPIC_DATA_PULL_ERROR,
                            "{\"requested\":\"all\",\"reason\":\"open_failed\"}", false);
            _uploadState = UploadState::IDLE;
        }
        return;
    }

    if (_uploadState == UploadState::READING) {
        // If there's still a buffered group from a failed previous publish,
        // try it again (don't read another row until this one drains).
        if (_group.count > 0 && _groupRetries > 0) {
            uploadPublishCurrentGroup();
            return;
        }
        if (uploadReadNextGroup()) {
            uploadPublishCurrentGroup();
        } else {
            // EOF and nothing more to flush
            _uploadState = UploadState::FINISHING;
            _uploadSucceeded = true;
        }
        return;
    }

    if (_uploadState == UploadState::FINISHING) {
        uploadFinish();
        _uploadState = UploadState::IDLE;
    }
}

bool MqttClient::uploadOpenFile() {
    if (s_uploadFileOpen) { s_uploadFile.close(); s_uploadFileOpen = false; }
    s_uploadFile = SD.open(LOG_FILE, FILE_READ);
    if (!s_uploadFile) return false;
    s_uploadFileOpen = true;
    // Consume header line
    String header = s_uploadFile.readStringUntil('\n');
    (void)header;
    memset(&_group, 0, sizeof(_group));
    s_lookaheadValid = false;
    Serial.println("[MQTT] Upload: file opened");
    return true;
}

// Parse one CSV line into the group buffer. If lineIsFirstOfGroup, populate
// the header/config/calibration slots; always append the spectra row.
// Returns false on malformed row (row skipped).
//
// New column layout (after R1 / clarification #6):
//   col 0  uuid           (RFC 4122 v4 string)
//   col 1  exp_id         (user label)
//   col 2  date
//   col 3  meas_idx
//   col 4  gain           (numeric AS7265X enum 0..3)
//   col 5  int_cycles
//   col 6  white_led      ("ON"/"OFF")
//   col 7  white_mA
//   col 8  ir_led         ("ON"/"OFF")
//   col 9  ir_mA
//   col 10 uv_led         ("ON"/"OFF")
//   col 11 uv_mA
//   col 12..29  cal_*18  (blank reference per channel)
//   col 30..47  ch_*18   (Δ counts per channel)
// → 12 metadata + 18 cal + 18 meas = 48 columns.
static bool parseRowIntoGroup(char* line, MqttClient::UploadGroup& g, bool isFirstOfGroup) {
    char* fields[64];
    int nf = splitCsv(line, fields, 64);
    if (nf < 48) return false;

    if (isFirstOfGroup) {
        strncpy(g.uuid,   fields[0], sizeof(g.uuid)   - 1); g.uuid[sizeof(g.uuid)   - 1] = 0;
        strncpy(g.exp_id, fields[1], sizeof(g.exp_id) - 1); g.exp_id[sizeof(g.exp_id) - 1] = 0;
        // gain column is now numeric (sd_logger writes int casted from enum).
        // atoi("16x") returns 16 — wrong; with the new schema we always get
        // a small int 0..3.  Defensive: if the field is empty fall back to 16x.
        g.gain_int     = (fields[4][0] >= '0' && fields[4][0] <= '9')
                         ? atoi(fields[4]) : gainStrToInt(fields[4]);
        g.int_cycles   = atoi(fields[5]);
        g.mode         = 3;
        g.led_white_en = (!strcmp(fields[6],  "ON"));
        g.led_white_ma = atoi(fields[7]);
        g.led_ir_en    = (!strcmp(fields[8],  "ON"));
        g.led_ir_ma    = atoi(fields[9]);
        g.led_uv_en    = (!strcmp(fields[10], "ON"));
        g.led_uv_ma    = atoi(fields[11]);

        bool anyNonZero = false;
        for (int i = 0; i < NUM_CHANNELS; i++) {
            g.offsets[i] = atof(fields[12 + i]);
            if (g.offsets[i] != 0.0f) anyNonZero = true;
        }
        g.cal_valid = anyNonZero;
        g.count = 0;
    }

    if (g.count < MAX_MEASUREMENTS) {
        for (int i = 0; i < NUM_CHANNELS; i++) {
            g.spectra[g.count][i] = atof(fields[30 + i]);
        }
        g.count++;
    }
    return true;
}

bool MqttClient::uploadReadNextGroup() {
    // If the previous pass left a lookahead row, that row starts the new group.
    if (s_lookaheadValid) {
        s_lookaheadValid = false;
        char buf[2048];
        s_lookaheadLine.toCharArray(buf, sizeof(buf));
        parseRowIntoGroup(buf, _group, true);
    } else {
        // No lookahead — clear group, we need to read a row that will seed it.
        _group.count = 0;
    }

    while (s_uploadFile.available()) {
        String line = s_uploadFile.readStringUntil('\n');
        if (line.length() == 0) continue;

        // Peek the uuid (column 0) without consuming the row: copy, split,
        // then decide whether it belongs to the current group.  Grouping by
        // uuid (R1) is rename-safe AND collision-safe: two consecutive
        // experiments named "agua" with different uuids will NOT be merged.
        char buf[2048];
        line.toCharArray(buf, sizeof(buf));
        char* fields[64];
        int nf = splitCsv(buf, fields, 64);
        if (nf < 48) continue;  // malformed, skip

        if (_group.count == 0) {
            char buf2[2048];
            line.toCharArray(buf2, sizeof(buf2));
            parseRowIntoGroup(buf2, _group, true);
            continue;
        }

        if (strcmp(fields[0], _group.uuid) != 0) {
            // New experiment — stash for next call, return current for publish.
            s_lookaheadLine = line;
            s_lookaheadValid = true;
            return true;
        }

        // Same uuid — append spectra row to current group.
        char buf3[2048];
        line.toCharArray(buf3, sizeof(buf3));
        parseRowIntoGroup(buf3, _group, false);
    }

    // EOF — return true if we have a group to flush, false otherwise.
    return _group.count > 0;
}

void MqttClient::uploadPublishCurrentGroup() {
    if (_group.count == 0) return;
    String json = buildGroupJson();
    size_t bytes = json.length();
    bool ok = _client.publish(MQTT_TOPIC_DATA_UPLOAD, json.c_str(), false);
    Serial.printf("[MQTT] upload exp='%s' uuid=%s count=%d bytes=%u %s\n",
                  _group.exp_id, _group.uuid, _group.count,
                  (unsigned)bytes, ok ? "ok" : "FAIL");

    if (!ok) {
        _groupRetries++;
        if (_groupRetries < UPLOAD_MAX_RETRIES) {
            // Likely causes: oversized packet (publish() returns false when
            // it can't fit), broker socket transient.  Keep _group buffered
            // and try again next tick.  We do NOT advance the file cursor
            // so no other experiment is touched in the meantime.
            Serial.printf("[MQTT] upload retry %d/%d for uuid=%s (bytes=%u)\n",
                          _groupRetries, UPLOAD_MAX_RETRIES,
                          _group.uuid, (unsigned)bytes);
            return;
        }
        // Persistent failure — give up on THIS experiment, surface it loudly,
        // and continue with the next one so a single bad payload can't wedge
        // the whole pull session.
        char err[192];
        snprintf(err, sizeof(err),
                 "{\"requested\":\"%s\",\"exp_id\":\"%s\",\"reason\":\"publish_failed\","
                 "\"bytes\":%u,\"buffer\":%u}",
                 _group.uuid, _group.exp_id,
                 (unsigned)bytes, (unsigned)MQTT_MAX_PACKET_SIZE);
        _client.publish(MQTT_TOPIC_DATA_PULL_ERROR, err, false);
        _uploadedFail++;
        Serial.printf("[MQTT] upload GIVE UP uuid=%s after %d attempts\n",
                      _group.uuid, _groupRetries);
        // The pending flag for this uuid stays — next pull will re-attempt.
        // Fall through to clear the buffer so the next group can be read.
    } else {
        _uploadedOk++;
        // Per-experiment progress event so the dashboard's banner can show
        // "uploading 3/10..." live, mirroring the local UI's progress bar.
        char prog[160];
        snprintf(prog, sizeof(prog),
                 "{\"phase\":\"upload\",\"uuid\":\"%s\",\"exp_id\":\"%s\","
                 "\"ok\":%d,\"fail\":%d}",
                 _group.uuid, _group.exp_id, _uploadedOk, _uploadedFail);
        _client.publish("esp32/data/upload/progress", prog, false);
    }
    // Clear so uploadReadNextGroup knows the buffer is empty for the next pass.
    _group.count   = 0;
    _groupRetries  = 0;
}

void MqttClient::uploadFinish() {
    if (s_uploadFileOpen) { s_uploadFile.close(); s_uploadFileOpen = false; }
    s_lookaheadValid = false;

    if (_uploadSucceeded) {
        // *** DO NOT delete /spectra.csv here. ***
        // A successful publish over MQTT only means the broker accepted the
        // packet — it does NOT mean the bridge ingested it into MySQL.  The
        // safe protocol is:
        //   1. saveExperiment() left a /pending/<uuid>.json flag (already done).
        //   2. We just finished publishing every CSV row to the broker.
        //   3. cleanupVerifiedExperiments() will (a) HTTP-GET /verify per
        //      pending exp, (b) drop only the verified exp's rows from the
        //      CSV via removeExperimentRows(), (c) clear the flag, and
        //      (d) remove the CSV entirely once /pending is empty.
        // Anything that fails verification keeps its flag and its CSV rows,
        // so the next pull_data re-publishes pending + new data.
        _client.publish(MQTT_TOPIC_DATA_STATE, "UPLOAD_COMPLETE", false);
    }
    // Final summary so the dashboard can show "uploaded N of M (K failed)".
    char done[192];
    snprintf(done, sizeof(done),
             "{\"ok\":%d,\"fail\":%d,\"buffer\":%u}",
             _uploadedOk, _uploadedFail, (unsigned)MQTT_MAX_PACKET_SIZE);
    _client.publish("esp32/data/upload/done", done, false);
    Serial.printf("[MQTT] bulk upload finished — ok=%d fail=%d (verify pass next)\n",
                  _uploadedOk, _uploadedFail);
}
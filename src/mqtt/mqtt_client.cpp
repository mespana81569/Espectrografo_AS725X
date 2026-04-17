#include "mqtt_client.h"

#include <ArduinoJson.h>
#include <SD.h>
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
        out += String(v[i], decimals);
    }
    out += "]";
}

// ─── Construction / lifecycle ───────────────────────────────────────────────

MqttClient::MqttClient()
    : _client(_wifiClient),
      _lastReconnectAttempt(0),
      _lastHeartbeat(0),
      _wasConnected(false),
      _pendingCalibrate(false),
      _pendingConfirm(false),
      _pendingAccept(false),
      _pendingSave(false),
      _pendingDiscard(false),
      _pendingConfig(false),
      _pendingConfigLen(0),
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

    // 4) Edge: first successful connect → subscribe + kick off SD upload.
    if (!_wasConnected) {
        _wasConnected = true;
        Serial.println("[MQTT] Connected to broker");
        subscribeAll();
        triggerSDUpload();
        // Publish current state so control.html sees a fresh snapshot.
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
    Serial.println("[MQTT] Subscribed to esp32/cmd/*");
}

void MqttClient::triggerSDUpload() {
    if (!g_sdLogger.isReady()) {
        Serial.println("[MQTT] SD not ready — skipping bulk upload");
        return;
    }
    if (!SD.exists(LOG_FILE)) {
        Serial.println("[MQTT] No /spectra.csv to upload");
        return;
    }
    _uploadState = UploadState::OPENING;
    _uploadSucceeded = false;
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
            // Mirror the REST /api/save flow so the behaviour is identical.
            bool ok = g_sdLogger.saveExperiment(g_measurementEngine.getExperiment());
            if (ok) publishExperiment(g_measurementEngine.getExperiment());
            g_stateMachine.requestTransition(SystemState::IDLE);
        }
    }

    if (_pendingDiscard) {
        _pendingDiscard = false;
        if (g_stateMachine.getState() == SystemState::SAVE_DECISION) {
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
                SensorConfig cfg = g_sensorDriver.getConfig();
                if (doc.containsKey("gain"))              cfg.gain              = (SensorGain)(uint8_t)doc["gain"];
                if (doc.containsKey("integrationCycles")) cfg.integrationCycles = doc["integrationCycles"];
                if (doc.containsKey("mode"))              cfg.mode              = (MeasurementMode)(uint8_t)doc["mode"];
                if (doc.containsKey("ledWhiteCurrent"))   cfg.ledWhiteCurrent   = doc["ledWhiteCurrent"];
                if (doc.containsKey("ledIrCurrent"))      cfg.ledIrCurrent      = doc["ledIrCurrent"];
                if (doc.containsKey("ledUvCurrent"))      cfg.ledUvCurrent      = doc["ledUvCurrent"];
                if (doc.containsKey("ledWhiteEnabled"))   cfg.ledWhiteEnabled   = doc["ledWhiteEnabled"];
                if (doc.containsKey("ledIrEnabled"))      cfg.ledIrEnabled      = doc["ledIrEnabled"];
                if (doc.containsKey("ledUvEnabled"))      cfg.ledUvEnabled      = doc["ledUvEnabled"];
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

// ─── Heartbeat ──────────────────────────────────────────────────────────────

void MqttClient::publishHeartbeat() {
    StaticJsonDocument<192> doc;
    doc["state"]    = g_stateMachine.getStateName();
    doc["rssi"]     = WiFi.RSSI();
    doc["heap"]     = (int)ESP.getFreeHeap();
    doc["sd_ready"] = g_sdLogger.isReady();
    char buf[192];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    _client.publish(MQTT_TOPIC_DATA_STATUS, (const uint8_t*)buf, n, false);
}

// ─── JSON builders ──────────────────────────────────────────────────────────

String MqttClient::buildExperimentJson(const Experiment& exp) {
    String out;
    out.reserve(4096);
    out += "{\"exp_id\":\"";
    out += exp.experiment_id;
    out += "\",\"timestamp_ms\":";
    out += (unsigned long)exp.timestamp;
    out += ",\"num_measurements\":";
    out += exp.num_measurements;
    out += ",\"sensor\":{\"gain\":";
    out += (int)(uint8_t)exp.sensor_cfg.gain;
    out += ",\"mode\":";
    out += (int)(uint8_t)exp.sensor_cfg.mode;
    out += ",\"int_cycles\":";
    out += (int)exp.sensor_cfg.integrationCycles;
    out += ",\"led_white_ma\":";
    out += exp.sensor_cfg.ledWhiteEnabled ? (int)exp.sensor_cfg.ledWhiteCurrent : 0;
    out += ",\"led_ir_ma\":";
    out += exp.sensor_cfg.ledIrEnabled    ? (int)exp.sensor_cfg.ledIrCurrent    : 0;
    out += ",\"led_uv_ma\":";
    out += exp.sensor_cfg.ledUvEnabled    ? (int)exp.sensor_cfg.ledUvCurrent    : 0;
    out += "},\"calibration\":{\"valid\":";
    out += exp.calibration.valid ? "true" : "false";
    out += ",\"offsets\":";
    appendFloatArray(out, exp.calibration.offset, NUM_CHANNELS);
    out += "},\"spectra\":[";
    for (int i = 0; i < exp.count; i++) {
        if (i) out += ",";
        appendFloatArray(out, exp.spectra[i], NUM_CHANNELS);
    }
    out += "],\"wavelengths_nm\":";
    appendWavelengths(out);
    out += "}";
    return out;
}

String MqttClient::buildGroupJson() const {
    String out;
    out.reserve(4096);
    out += "{\"exp_id\":\"";
    out += _group.exp_id;
    out += "\",\"timestamp_ms\":0,\"num_measurements\":";
    out += _group.count;
    out += ",\"sensor\":{\"gain\":";
    out += _group.gain_int;
    out += ",\"mode\":";
    out += _group.mode;
    out += ",\"int_cycles\":";
    out += _group.int_cycles;
    out += ",\"led_white_ma\":";
    out += _group.led_white_en ? _group.led_white_ma : 0;
    out += ",\"led_ir_ma\":";
    out += _group.led_ir_en    ? _group.led_ir_ma    : 0;
    out += ",\"led_uv_ma\":";
    out += _group.led_uv_en    ? _group.led_uv_ma    : 0;
    out += "},\"calibration\":{\"valid\":";
    out += _group.cal_valid ? "true" : "false";
    out += ",\"offsets\":";
    appendFloatArray(out, _group.offsets, NUM_CHANNELS);
    out += "},\"spectra\":[";
    for (int i = 0; i < _group.count; i++) {
        if (i) out += ",";
        appendFloatArray(out, _group.spectra[i], NUM_CHANNELS);
    }
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
            _uploadState = UploadState::IDLE;
        }
        return;
    }

    if (_uploadState == UploadState::READING) {
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
static bool parseRowIntoGroup(char* line, MqttClient::UploadGroup& g, bool isFirstOfGroup) {
    char* fields[64];
    int nf = splitCsv(line, fields, 64);
    // Expected columns: 11 metadata + 18 cal + 18 meas = 47
    if (nf < 47) return false;

    if (isFirstOfGroup) {
        strncpy(g.exp_id, fields[1], sizeof(g.exp_id) - 1);
        g.exp_id[sizeof(g.exp_id) - 1] = 0;
        g.gain_int     = gainStrToInt(fields[3]);
        g.int_cycles   = atoi(fields[4]);
        g.mode         = 3;  // not stored in CSV; Mode 3 is the only one logged
        g.led_white_en = (!strcmp(fields[5], "ON"));
        g.led_white_ma = atoi(fields[6]);
        g.led_ir_en    = (!strcmp(fields[7], "ON"));
        g.led_ir_ma    = atoi(fields[8]);
        g.led_uv_en    = (!strcmp(fields[9], "ON"));
        g.led_uv_ma    = atoi(fields[10]);

        bool anyNonZero = false;
        for (int i = 0; i < NUM_CHANNELS; i++) {
            g.offsets[i] = atof(fields[11 + i]);
            if (g.offsets[i] != 0.0f) anyNonZero = true;
        }
        g.cal_valid = anyNonZero;
        g.count = 0;
    }

    if (g.count < MAX_MEASUREMENTS) {
        for (int i = 0; i < NUM_CHANNELS; i++) {
            g.spectra[g.count][i] = atof(fields[29 + i]);
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

        // Peek the exp_id of this row without consuming it first: copy, split,
        // then decide whether it belongs to the current group.
        char buf[2048];
        line.toCharArray(buf, sizeof(buf));
        char* fields[64];
        int nf = splitCsv(buf, fields, 64);
        if (nf < 47) continue;  // malformed, skip

        if (_group.count == 0) {
            // Fresh group — seed from this row.
            char buf2[2048];
            line.toCharArray(buf2, sizeof(buf2));
            parseRowIntoGroup(buf2, _group, true);
            continue;
        }

        if (strcmp(fields[1], _group.exp_id) != 0) {
            // New experiment — stash for next call, return current for publish.
            s_lookaheadLine = line;
            s_lookaheadValid = true;
            return true;
        }

        // Same exp_id — append spectra row to current group.
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
    bool ok = _client.publish(MQTT_TOPIC_DATA_UPLOAD, json.c_str(), false);
    Serial.printf("[MQTT] upload exp='%s' count=%d bytes=%u %s\n",
                  _group.exp_id, _group.count,
                  (unsigned)json.length(), ok ? "ok" : "FAIL");
    // Clear so uploadReadNextGroup knows the buffer is empty for the next pass.
    _group.count = 0;
}

void MqttClient::uploadFinish() {
    if (s_uploadFileOpen) { s_uploadFile.close(); s_uploadFileOpen = false; }
    s_lookaheadValid = false;

    if (_uploadSucceeded) {
        if (SD.remove(LOG_FILE)) {
            Serial.println("[MQTT] /spectra.csv deleted after upload");
        } else {
            Serial.println("[MQTT] WARNING: could not delete /spectra.csv");
        }
        // Re-create the empty CSV with header so future saves still work.
        g_sdLogger.ensureHeader();
        _client.publish(MQTT_TOPIC_DATA_STATE, "UPLOAD_COMPLETE", false);
    }
    Serial.println("[MQTT] bulk upload finished");
}
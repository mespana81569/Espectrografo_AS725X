#pragma once

// PubSubClient uses this to size its internal RX/TX buffer.
// Must be defined BEFORE including PubSubClient.h anywhere in the translation
// unit — one experiment payload can reach ~3.8 KB.
#ifndef MQTT_MAX_PACKET_SIZE
#define MQTT_MAX_PACKET_SIZE 4096
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include "../acquisition/measurement_engine.h"  // Experiment struct

// ─── Broker configuration ───────────────────────────────────────────────────
// PubSubClient expects a plain hostname (no scheme, no brackets).
#define MQTT_BROKER_HOST   "10.124.102.52"
//#define MQTT_BROKER_HOST   "cygnus.uniajc.edu.co"
#define MQTT_BROKER_PORT   1884
#define MQTT_CLIENT_ID     "espectrografo-01"

// ─── Topic names ────────────────────────────────────────────────────────────
// Commands (subscribed) — control.html → ESP32
#define MQTT_TOPIC_CMD_CALIBRATE "esp32/cmd/calibrate"
#define MQTT_TOPIC_CMD_CONFIRM   "esp32/cmd/confirm"
#define MQTT_TOPIC_CMD_ACCEPT    "esp32/cmd/accept"
#define MQTT_TOPIC_CMD_SAVE      "esp32/cmd/save"
#define MQTT_TOPIC_CMD_DISCARD   "esp32/cmd/discard"
#define MQTT_TOPIC_CMD_CONFIG    "esp32/cmd/config"

// Data (published) — ESP32 → control.html
#define MQTT_TOPIC_DATA_STATE    "esp32/data/state"
#define MQTT_TOPIC_DATA_SPECTRA  "esp32/data/spectra"
#define MQTT_TOPIC_DATA_STATUS   "esp32/data/status"
#define MQTT_TOPIC_DATA_UPLOAD   "esp32/data/upload"

class MqttClient {
public:
    MqttClient();

    // Called once from setup() after sensor/SD init.
    void begin();

    // Called every loop() iteration on Core 1.
    // Handles reconnect, pumps PubSubClient, processes deferred commands,
    // drives the SD bulk upload state machine and publishes heartbeat.
    void tick();

    bool isConnected();

    // Low-level publish wrapper — returns true on success.
    bool publish(const char* topic, const char* payload, bool retained = false);
    bool publish(const char* topic, const char* payload, uint8_t qos, bool retained);

    // Publish current SystemState name (called from StateMachine::enterState).
    // Safe to call before connect — becomes a no-op.
    void publishState(const char* stateName);

    // Publish full experiment JSON to esp32/data/spectra (after SAVE_DECISION
    // completes with a successful SD write).
    void publishExperiment(const Experiment& exp);

    // Reconstructed experiment group, held across upload ticks.
    // Public only so file-local upload helpers in the .cpp can take it by
    // reference — not part of the stable API surface.
    struct UploadGroup {
        char  exp_id[32];
        int   gain_int;          // 0-3 decoded from "1x"/"4x"/"16x"/"64x"
        int   int_cycles;
        int   mode;              // CSV has no mode column; defaults to 3
        int   led_white_ma;
        int   led_ir_ma;
        int   led_uv_ma;
        bool  led_white_en;
        bool  led_ir_en;
        bool  led_uv_en;
        float offsets[NUM_CHANNELS];
        bool  cal_valid;
        int   count;
        float spectra[MAX_MEASUREMENTS][NUM_CHANNELS];
    };

private:
    WiFiClient   _wifiClient;
    PubSubClient _client;

    // Reconnect backoff (no delay() — all millis() driven)
    unsigned long _lastReconnectAttempt;
    static const unsigned long RECONNECT_INTERVAL_MS = 5000;

    // Heartbeat cadence
    unsigned long _lastHeartbeat;
    static const unsigned long HEARTBEAT_INTERVAL_MS = 5000;

    bool _wasConnected;  // edge-detect first successful connect

    // ─── Deferred command buffers ─────────────────────────────────────────
    // The PubSubClient callback runs synchronously from within _client.loop().
    // We never do sensor/SD/blocking work from the callback — instead we raise
    // a flag (or copy a payload) and process it in tick() on the next pass.
    volatile bool _pendingCalibrate;
    volatile bool _pendingConfirm;
    volatile bool _pendingAccept;
    volatile bool _pendingSave;
    volatile bool _pendingDiscard;

    volatile bool _pendingConfig;
    size_t        _pendingConfigLen;
    char          _pendingConfigBuf[768];

    // ─── SD bulk upload state ─────────────────────────────────────────────
    enum class UploadState : uint8_t {
        IDLE,
        OPENING,
        READING,
        FINISHING
    };
    UploadState _uploadState;
    UploadGroup _group;
    bool        _uploadSucceeded;

    // ─── Private helpers ──────────────────────────────────────────────────
    static MqttClient* _instance;
    static void staticCallback(char* topic, uint8_t* payload, unsigned int len);
    void handleCallback(const char* topic, const uint8_t* payload, unsigned int len);

    bool attemptConnect();
    void subscribeAll();
    void triggerSDUpload();

    // Command dispatch
    void processPendingCommands();

    // Heartbeat publisher
    void publishHeartbeat();

    // Bulk upload driver — runs at most one "unit" of work per call
    void processUploadTick();
    bool uploadOpenFile();
    bool uploadReadNextGroup();    // returns false on EOF with nothing buffered
    void uploadPublishCurrentGroup();
    void uploadFinish();            // delete file, publish UPLOAD_COMPLETE

    // JSON builders (String — heap; ESP32 has plenty)
    static String buildExperimentJson(const Experiment& exp);
    String buildGroupJson() const;
};

extern MqttClient g_mqttClient;
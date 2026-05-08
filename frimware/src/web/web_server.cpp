#include "web_server.h"
#include "api_routes.h"
#include "../ui/html_content.h"
#include <WiFi.h>
#include <Arduino.h>

AsyncWebServer g_httpServer(80);

static const char* AP_SSID = "Espectrografo-AP";
static const char* AP_PASS = "esp32spectro";

// ─── WiFi STA connection state ───────────────────────────────────────────────
static char     s_staSSID[64]  = {0};
static char     s_staPASS[64]  = {0};
static bool     s_connectReq   = false;
static bool     s_connecting   = false;
static bool     s_staEnabled   = false;
static uint32_t s_connectStart = 0;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;  // full window before giving up

// ─── WiFi scan state machine (runs from loop on Core 1) ─────────────────────
enum class ScanStep : uint8_t {
    IDLE,           // nothing happening
    REQUESTED,      // API set flag, waiting for loop to pick it up
    RADIO_OFF,      // WiFi.mode(WIFI_OFF) done, settling
    STA_INIT,       // WiFi.mode(WIFI_STA) done, settling
    SCANNING,       // scanNetworks() running (synchronous, blocks)
    RESTORING_AP,   // AP restarted, settling
    DONE            // results cached, ready to serve
};
static volatile ScanStep s_scanStep = ScanStep::IDLE;
static uint32_t s_scanTimer = 0;
static volatile bool s_hasResults = false;

// Fixed-size result storage — populated during SCANNING, before any WiFi mode
// change that could invalidate the driver's internal scan list or fragment heap.
struct ScanNet {
    char     ssid[33];   // 32 + null
    int16_t  rssi;
    uint8_t  enc;        // 0 = open, 1 = secured
};
static const int MAX_SCAN_RESULTS = 20;
static ScanNet s_scanResults[MAX_SCAN_RESULTS];
static volatile int s_scanResultCount = 0;

// ─── Public WiFi functions ───────────────────────────────────────────────────

void wifiRequestConnect(const char* ssid, const char* pass) {
    strlcpy(s_staSSID, ssid, sizeof(s_staSSID));
    strlcpy(s_staPASS, pass, sizeof(s_staPASS));
    s_connectReq = true;
}

String wifiStaStatus() {
    if (s_connecting) return "connecting";
    if (!s_staEnabled) return "not_configured";
    wl_status_t st = WiFi.status();
    switch (st) {
        case WL_CONNECTED:      return "connected:" + WiFi.localIP().toString();
        case WL_CONNECT_FAILED: return "failed";
        case WL_NO_SSID_AVAIL:  return "ssid_not_found";
        case WL_DISCONNECTED:   return "disconnected";
        default:                return "disconnected";
    }
}

void wifiRequestScan() {
    if (s_scanStep == ScanStep::IDLE || s_scanStep == ScanStep::DONE) {
        s_scanStep = ScanStep::REQUESTED;
        s_hasResults = false;
        s_scanResultCount = 0;
        Serial.println("[WiFi] Scan requested");
    }
}

bool wifiScanInProgress() {
    return s_scanStep != ScanStep::IDLE && s_scanStep != ScanStep::DONE;
}

bool wifiScanHasResults() {
    return s_hasResults;
}

String wifiScanResultsJson() {
    // Build JSON on demand from the fixed copy — no heap state shared with the
    // WiFi driver, so mode cycling cannot corrupt it.
    String json = "{\"networks\":[";
    int count = s_scanResultCount;
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"";
        const char* s = s_scanResults[i].ssid;
        for (int c = 0; s[c] != 0; c++) {
            char ch = s[c];
            if (ch == '"')       json += "\\\"";
            else if (ch == '\\') json += "\\\\";
            else                 json += ch;
        }
        json += "\",\"rssi\":";
        json += (int)s_scanResults[i].rssi;
        json += ",\"enc\":";
        json += s_scanResults[i].enc ? "true" : "false";
        json += "}";
    }
    json += "]}";
    return json;
}

// ─── Scan tick — called from loop() on Core 1 ───────────────────────────────
void wifiScanTick() {
    switch (s_scanStep) {
        case ScanStep::REQUESTED: {
            Serial.println("[WiFi] Shutting down AP + HTTP...");
            g_httpServer.end();
            WiFi.softAPdisconnect(true);
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            s_scanTimer = millis();
            s_scanStep = ScanStep::RADIO_OFF;
            break;
        }
        case ScanStep::RADIO_OFF: {
            if (millis() - s_scanTimer < 500) break;
            Serial.println("[WiFi] Entering STA mode...");
            WiFi.mode(WIFI_STA);
            WiFi.disconnect(false);
            s_scanTimer = millis();
            s_scanStep = ScanStep::STA_INIT;
            break;
        }
        case ScanStep::STA_INIT: {
            if (millis() - s_scanTimer < 1000) break;
            Serial.println("[WiFi] Scanning...");
            s_scanStep = ScanStep::SCANNING;

            int n = WiFi.scanNetworks(false, false, false, 300);
            Serial.printf("[WiFi] Found %d networks\n", n);

            // Copy results into fixed struct array BEFORE any mode change.
            int stored = 0;
            for (int i = 0; i < n && stored < MAX_SCAN_RESULTS; i++) {
                String ssid = WiFi.SSID(i);
                if (ssid.length() == 0) continue;

                bool dup = false;
                for (int k = 0; k < stored; k++) {
                    if (ssid.equals(s_scanResults[k].ssid)) { dup = true; break; }
                }
                if (dup) continue;

                strlcpy(s_scanResults[stored].ssid, ssid.c_str(),
                        sizeof(s_scanResults[stored].ssid));
                s_scanResults[stored].rssi = (int16_t)WiFi.RSSI(i);
                s_scanResults[stored].enc  =
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? 0 : 1;

                Serial.printf("  %s  %d dBm  %s\n",
                    s_scanResults[stored].ssid,
                    (int)s_scanResults[stored].rssi,
                    s_scanResults[stored].enc ? "enc" : "open");
                stored++;
            }
            s_scanResultCount = stored;
            WiFi.scanDelete();

            // STA → AP directly to avoid double netstack init error
            Serial.println("[WiFi] Restoring AP...");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_AP);
            WiFi.softAP(AP_SSID, AP_PASS);
            s_scanTimer = millis();
            s_scanStep = ScanStep::RESTORING_AP;
            break;
        }
        case ScanStep::RESTORING_AP: {
            if (millis() - s_scanTimer < 1500) break;
            g_httpServer.begin();
            s_hasResults = true;
            s_scanStep = ScanStep::DONE;
            Serial.printf("[WiFi] AP ready at %s — %d results cached\n",
                          WiFi.softAPIP().toString().c_str(), (int)s_scanResultCount);
            break;
        }
        default:
            break;
    }
}

// ─── Setup ───────────────────────────────────────────────────────────────────

void webServerSetup() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[WiFi] AP '%s' up at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    g_httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", HTML_CONTENT);
    });

    registerApiRoutes(g_httpServer);

    g_httpServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });

    g_httpServer.begin();
    Serial.println("[HTTP] Server started on port 80");
}

// ─── Loop ────────────────────────────────────────────────────────────────────

// ─── AP restore helper ───────────────────────────────────────────────────────
static void restoreAP() {
    Serial.println("[WiFi] Restoring AP...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(500);
    g_httpServer.begin();
    Serial.printf("[WiFi] AP back at %s\n", WiFi.softAPIP().toString().c_str());
    s_staEnabled = false;
    s_connecting = false;
}

void webServerLoop() {
    wifiScanTick();

    if (s_connectReq) {
        s_connectReq = false;
        s_connecting = true;
        s_staEnabled = true;
        s_connectStart = millis();

        // Drop AP, go STA-only for a clean connection attempt
        Serial.printf("[WiFi] Dropping AP, connecting to '%s'...\n", s_staSSID);
        g_httpServer.end();
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        delay(1000);          // give radio time to fully settle in STA mode
        WiFi.begin(s_staSSID, s_staPASS);
        Serial.printf("[WiFi] Attempting connection (up to %lus)...\n",
                      (unsigned long)(CONNECT_TIMEOUT_MS / 1000));
    }

    if (s_connecting) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            // Success — finalise teardown of the AP and come back online on STA.
            s_connecting = false;
            WiFi.softAPdisconnect(true);   // make the AP gone for good
            WiFi.mode(WIFI_STA);            // STA-only
            g_httpServer.begin();           // bring REST API back on STA IP
            Serial.printf("[WiFi] Connected! IP: %s — AP torn down, HTTP re-listening\n",
                          WiFi.localIP().toString().c_str());
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            Serial.println("[WiFi] NTP sync requested");
        } else if ((millis() - s_connectStart) > CONNECT_TIMEOUT_MS) {
            // Only give up after the full window — do NOT bail on WL_CONNECT_FAILED
            // early, as the driver can bounce through that state and recover.
            Serial.printf("[WiFi] 15s timeout — last status=%d — restoring AP\n", (int)st);
            restoreAP();
        }
    }
}

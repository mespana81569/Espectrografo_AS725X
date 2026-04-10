#include "web_server.h"
#include "api_routes.h"
#include "../ui/html_content.h"
#include <WiFi.h>
#include <Arduino.h>

AsyncWebServer g_httpServer(80);

static const char* AP_SSID = "Espectrografo-AP";
static const char* AP_PASS = "esp32spectro";

// ─── WiFi STA state (written from API handler, read in loop) ─────────────────
static char   s_staSSID[64]  = {0};
static char   s_staPASS[64]  = {0};
static bool   s_connectReq   = false;  // set by API, cleared by loop
static bool   s_connecting   = false;
static bool   s_staEnabled   = false;  // true once user has tried to connect
static uint32_t s_connectStart = 0;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;

// ─── Cached scan results ─────────────────────────────────────────────────────
struct ScannedNet {
    char ssid[33];
    int8_t rssi;
    bool encrypted;
};
static ScannedNet s_nets[20];
static int s_netCount = 0;
static String s_cachedJson = "{\"scanning\":false,\"networks\":[]}";
static bool s_scanBusy = false;

static void storeScanResults() {
    int n = WiFi.scanComplete();
    if (n <= 0) { s_netCount = 0; return; }

    s_netCount = 0;
    for (int i = 0; i < n && s_netCount < 20; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        // Deduplicate
        bool dup = false;
        for (int j = 0; j < s_netCount; j++) {
            if (strcmp(s_nets[j].ssid, ssid.c_str()) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strlcpy(s_nets[s_netCount].ssid, ssid.c_str(), 33);
        s_nets[s_netCount].rssi = WiFi.RSSI(i);
        s_nets[s_netCount].encrypted = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        s_netCount++;
    }

    // Debug: print to serial
    Serial.printf("[WiFi] Scan found %d networks (%d unique):\n", n, s_netCount);
    for (int i = 0; i < s_netCount; i++) {
        Serial.printf("  %s (%d dBm) %s\n", s_nets[i].ssid, s_nets[i].rssi,
                       s_nets[i].encrypted ? "enc" : "open");
    }
    WiFi.scanDelete();
}

static void buildScanJson() {
    String json = "{\"scanning\":false,\"networks\":[";
    for (int i = 0; i < s_netCount; i++) {
        if (i) json += ",";
        json += "{\"ssid\":\"";
        for (int c = 0; s_nets[i].ssid[c]; c++) {
            if (s_nets[i].ssid[c] == '"') json += "\\\"";
            else json += s_nets[i].ssid[c];
        }
        json += "\",\"rssi\":";
        json += s_nets[i].rssi;
        json += ",\"enc\":";
        json += s_nets[i].encrypted ? "true" : "false";
        json += "}";
    }
    json += "]}";
    s_cachedJson = json;
}

// Public accessors
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
        default:                return "disconnected";
    }
}

void wifiStartScan() {
    if (s_scanBusy) return;
    s_scanBusy = true;

    // Ensure we're in AP+STA mode so we can scan while keeping clients connected
    if (WiFi.getMode() == WIFI_AP) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(AP_SSID, AP_PASS);  // re-init AP after mode change
        delay(100);
    }
    WiFi.scanDelete();                    // clear any stale results
    WiFi.scanNetworks(true, false, false, 300);  // async scan
    Serial.println("[WiFi] Async scan started (AP stays up)");
}

bool wifiScanBusy() {
    return s_scanBusy;
}

String wifiScanResultsJson() {
    return s_cachedJson;
}

// ─── Boot-time scan (called from setup, before AP starts) ────────────────────
static void bootScan() {
    Serial.println("[WiFi] Boot scan — STA mode, no AP interference...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    delay(500);  // let STA interface initialize

    int n = WiFi.scanNetworks(false, false, false, 300);  // synchronous
    Serial.printf("[WiFi] Boot scan raw result: %d\n", n);

    storeScanResults();
    buildScanJson();

    WiFi.mode(WIFI_OFF);   // clean slate before starting AP
    delay(100);
}

void webServerSetup() {
    // Scan FIRST while radio is free — results available when user opens page
    bootScan();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[WiFi] AP '%s' started, IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());

    // Serve main HTML page
    g_httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", HTML_CONTENT);
    });

    // Register all /api/* routes
    registerApiRoutes(g_httpServer);

    // 404 handler
    g_httpServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });

    g_httpServer.begin();
    Serial.println("[HTTP] Server started on port 80");
}

void webServerLoop() {
    // ── Check async scan completion (from wifiStartScan) ─────────────────────
    if (s_scanBusy) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            // still scanning — do nothing
        } else {
            // Done (n >= 0) or failed (n == -2)
            if (n >= 0) {
                storeScanResults();
                buildScanJson();
            } else {
                Serial.printf("[WiFi] Async scan failed (code %d)\n", n);
            }
            s_scanBusy = false;
            Serial.printf("[WiFi] Scan done, %d networks cached\n", s_netCount);
        }
    }

    // ── STA connection state machine ─────────────────────────────────────────
    if (s_connectReq) {
        s_connectReq = false;
        s_connecting = true;
        s_staEnabled = true;
        s_connectStart = millis();

        // Switch to AP+STA for internet access
        if (WiFi.getMode() != WIFI_AP_STA) {
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAP(AP_SSID, AP_PASS);
        }
        WiFi.disconnect(false);
        WiFi.begin(s_staSSID, s_staPASS);
        Serial.printf("[WiFi] Connecting to '%s'...\n", s_staSSID);
    }

    if (s_connecting) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            s_connecting = false;
            Serial.printf("[WiFi] Connected! STA IP: %s\n", WiFi.localIP().toString().c_str());
            // Sync real-time clock via NTP (UTC — adjust offset if needed)
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            Serial.println("[WiFi] NTP time sync requested");
        } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
                   (millis() - s_connectStart) > CONNECT_TIMEOUT_MS) {
            s_connecting = false;
            Serial.printf("[WiFi] Connection to '%s' failed (status=%d)\n", s_staSSID, st);
        }
    }
}

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
static uint32_t s_connectStart = 0;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;

// Public accessors for api_routes.cpp
void wifiRequestConnect(const char* ssid, const char* pass) {
    strlcpy(s_staSSID, ssid, sizeof(s_staSSID));
    strlcpy(s_staPASS, pass, sizeof(s_staPASS));
    s_connectReq = true;
}

String wifiStaStatus() {
    wl_status_t st = WiFi.status();
    switch (st) {
        case WL_CONNECTED:          return "connected:" + WiFi.localIP().toString();
        case WL_DISCONNECTED:       return "disconnected";
        case WL_CONNECT_FAILED:     return "failed";
        case WL_NO_SSID_AVAIL:      return "no_ssid";
        case WL_IDLE_STATUS:        return s_connecting ? "connecting" : "idle";
        default:                    return "idle";
    }
}

void webServerSetup() {
    WiFi.mode(WIFI_AP_STA);   // AP always on; STA used when credentials supplied
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
    // Non-blocking STA connection state machine
    if (s_connectReq) {
        s_connectReq = false;
        s_connecting = true;
        s_connectStart = millis();
        WiFi.disconnect(false);
        WiFi.begin(s_staSSID, s_staPASS);
        Serial.printf("[WiFi] Connecting to '%s'...\n", s_staSSID);
    }

    if (s_connecting) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            s_connecting = false;
            Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
                   (millis() - s_connectStart) > CONNECT_TIMEOUT_MS) {
            s_connecting = false;
            Serial.println("[WiFi] Connection failed or timed out");
        }
    }
}

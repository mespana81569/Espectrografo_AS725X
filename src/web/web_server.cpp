#include "web_server.h"
#include "api_routes.h"
#include "../ui/html_content.h"
#include <WiFi.h>
#include <Arduino.h>

AsyncWebServer g_httpServer(80);

static const char* AP_SSID = "Espectrografo-AP";
static const char* AP_PASS = "esp32spectro";

void webServerSetup() {
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
    // ESPAsyncWebServer is interrupt-driven; nothing needed here.
    // Reserved for future keepalive / mDNS tasks.
}

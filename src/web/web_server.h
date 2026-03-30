#pragma once
#include <ESPAsyncWebServer.h>
#include <Arduino.h>

void webServerSetup();
void webServerLoop();  // call from loop — handles non-blocking WiFi STA connection

void   wifiRequestConnect(const char* ssid, const char* pass);
String wifiStaStatus();

extern AsyncWebServer g_httpServer;

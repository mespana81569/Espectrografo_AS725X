#pragma once
#include <ESPAsyncWebServer.h>
#include <Arduino.h>

void webServerSetup();
void webServerLoop();  // call from loop — handles non-blocking WiFi STA connection + scan

void   wifiRequestConnect(const char* ssid, const char* pass);
String wifiStaStatus();

// WiFi scan (runs in loop, drops AP temporarily)
void   wifiStartScan();
bool   wifiScanBusy();
String wifiScanResultsJson();  // returns cached JSON from last completed scan

extern AsyncWebServer g_httpServer;

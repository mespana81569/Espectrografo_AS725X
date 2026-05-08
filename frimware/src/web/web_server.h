#pragma once
#include <ESPAsyncWebServer.h>
#include <Arduino.h>

void webServerSetup();
void webServerLoop();

void   wifiRequestConnect(const char* ssid, const char* pass);
String wifiStaStatus();

// WiFi scan — driven from loop() on Core 1
void   wifiRequestScan();                 // set by API handler
bool   wifiScanInProgress();              // true while scanning
bool   wifiScanHasResults();              // true after scan completed
String wifiScanResultsJson();             // cached JSON
void   wifiScanTick();                    // call from loop()

extern AsyncWebServer g_httpServer;

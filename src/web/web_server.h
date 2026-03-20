#pragma once
#include <ESPAsyncWebServer.h>

void webServerSetup();
void webServerLoop();  // call from loop for any periodic tasks (keepalive etc.)

extern AsyncWebServer g_httpServer;

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <ESPAsyncWebServer.h>

extern AsyncWebServer server;

void startWebServer();
String getSettings();
bool saveSettings(String json);
void processPendingNvsSave();

#endif

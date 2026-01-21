#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <ESPAsyncWebServer.h>

extern AsyncWebServer server;

void startWebServer();
String getSettings();
bool saveSettings(String json);

// Forward declarations for MQTT functions
bool isMqttConfigured();
const char* getMqttStatus();
void setMqttConfig(const String& server, int port, const String& user, const String& password, const String& topicStatus, const String& topicControl, const String& security);

#endif

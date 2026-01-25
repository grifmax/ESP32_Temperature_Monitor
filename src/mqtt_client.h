#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>

void initMqtt();
void setMqttConfig(const String& server, int port, const String& user, const String& password, const String& topicStatus, const String& topicControl, const String& security);
void disableMqtt(); // Принудительное отключение MQTT
void updateMqtt();
bool isMqttConfigured();
bool isMqttConnected();
const char* getMqttStatus();
bool sendMqttTestMessage();
bool sendMqttMetrics(unsigned long uptime, float temperature, const String& ip, int rssi);

#endif

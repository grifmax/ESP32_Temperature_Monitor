#ifndef WIFI_POWER_H
#define WIFI_POWER_H

#include <WiFi.h>
// #include <WiFiManager.h>  // Временно отключено

// extern WiFiManager wm;  // Временно отключено

void initWiFiPower();
void enableWiFi();
void disableWiFi();
bool isWiFiEnabled();
bool isAPMode(); // Проверка, находится ли устройство в режиме точки доступа
void setAPMode(bool enabled); // Установка режима точки доступа
bool startAccessPoint(const char* ssid, const char* password); // Запуск точки доступа
void requestWiFiOn(); // Запрос на включение WiFi (для локального режима)
void updateWiFiPower(); // Вызывать в loop для управления питанием

#endif

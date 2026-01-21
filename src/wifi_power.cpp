#include "wifi_power.h"
#include "config.h"
#include "operation_modes.h"
#include <Arduino.h>
#include <WiFi.h>

bool wifiEnabled = false;
bool apMode = false;
bool wifiRequested = false;
unsigned long wifiRequestTime = 0;
unsigned long wifiTimeout = 60000; // 60 секунд таймаут

void initWiFiPower() {
  wifiEnabled = false;
  apMode = false;
  wifiRequested = false;
}

void enableWiFi() {
  if (wifiEnabled || apMode) {
    return;
  }
  
  WiFi.mode(WIFI_STA);
  wifiEnabled = true;
  wifiRequested = false;
}

void disableWiFi() {
  if (apMode || !wifiEnabled) {
    return;
  }
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiEnabled = false;
  wifiRequested = false;
}

bool isWiFiEnabled() {
  return wifiEnabled || apMode;
}

bool isAPMode() {
  return apMode;
}

void setAPMode(bool enabled) {
  apMode = enabled;
  if (enabled) {
    wifiEnabled = true;
  }
}

bool startAccessPoint(const char* ssid, const char* password) {
  Serial.println(F("Starting Access Point..."));
  
  WiFi.mode(WIFI_AP);
  delay(100);
  
  bool apStarted = WiFi.softAP(ssid, password);
  
  if (apStarted) {
    delay(500);
    IPAddress apIP = WiFi.softAPIP();
    
    if (apIP.toString() != "0.0.0.0") {
      setAPMode(true);
      Serial.print(F("AP started! SSID: "));
      Serial.print(ssid);
      Serial.print(F(", IP: "));
      Serial.println(apIP);
      return true;
    }
    
    // Ждем назначения IP
    for (int i = 0; i < 10; i++) {
      delay(200);
      apIP = WiFi.softAPIP();
      if (apIP.toString() != "0.0.0.0") {
        setAPMode(true);
        Serial.print(F("AP started! IP: "));
        Serial.println(apIP);
        return true;
      }
    }
  }
  
  Serial.println(F("Failed to start AP"));
  return false;
}

void requestWiFiOn() {
  wifiRequested = true;
  wifiRequestTime = millis();
  enableWiFi();
}

void updateWiFiPower() {
  OperationMode mode = getOperationMode();
  
  if (apMode) {
    return;
  }
  
  if (mode == MODE_LOCAL) {
    if (wifiRequested) {
      if (millis() - wifiRequestTime > wifiTimeout) {
        disableWiFi();
      }
    } else {
      if (wifiEnabled && !apMode) {
        disableWiFi();
      }
    }
  } else {
    if (!wifiEnabled) {
      enableWiFi();
    }
    wifiRequested = false;
  }
}

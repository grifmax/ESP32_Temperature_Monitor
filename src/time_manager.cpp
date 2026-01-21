#include "time_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "config.h"

// Переменные для времени
int timezoneOffset = 3; // По умолчанию UTC+3 (Москва)
bool timeInitialized = false;
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0; // Будем использовать timezoneOffset
const int daylightOffset_sec = 0;

void setTimezone(int offset) {
  timezoneOffset = offset;
  if (timeInitialized) {
    configTime(timezoneOffset * 3600, daylightOffset_sec, ntpServer);
  }
}

int getTimezone() {
  return timezoneOffset;
}

void initTimeManager() {
  if (WiFi.status() == WL_CONNECTED) {
    configTime(timezoneOffset * 3600, daylightOffset_sec, ntpServer);
    timeInitialized = true;
    Serial.println(F("Time manager initialized"));
  }
}

void updateTime() {
  if (WiFi.status() == WL_CONNECTED && !timeInitialized) {
    initTimeManager();
  }
}

String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "--:--:--";
  }
  
  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  return String(timeStr);
}

String getCurrentDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "--/--/----";
  }
  
  char dateStr[20];
  strftime(dateStr, sizeof(dateStr), "%d.%m.%Y", &timeinfo);
  return String(dateStr);
}

unsigned long getUnixTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return 0;
  }
  return mktime(&timeinfo);
}

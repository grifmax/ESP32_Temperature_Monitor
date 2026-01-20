#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <U8g2lib.h>
#include <LittleFS.h>
#include "config.h"
#include "web_server.h"
#include "tg_bot.h"
#include "sensors.h"
#include "display.h"

// Инициализация объектов
WiFiManager wm;
OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature sensors(&oneWire);
U8G2_SSD1306_128X64_NONAME_1_HW_I2C display(U8G2_R0, OLED_SDA_PIN, OLED_SCL_PIN);

float currentTemp = 0.0;
float targetTemp = 25.0;
unsigned long lastSensorUpdate = 0;
unsigned long lastTelegramUpdate = 0;

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount file system.");
    return;
  }
  display.begin();
  display.setFont(u8g2_font_ncenB08_tr);

  wm.autoConnect("ESP32_Thermo", "password");
  sensors.begin();
  startWebServer();
  startTelegramBot();
}

void loop() {
  if (millis() - lastSensorUpdate > 10000) {
    readTemperature();
    lastSensorUpdate = millis();
  }
  if (millis() - lastTelegramUpdate > 1000) {
    handleTelegramMessages();
    lastTelegramUpdate = millis();
  }
  updateDisplay();
}

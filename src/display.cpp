#include "display.h"
#include "config.h"
#include <WiFi.h>
#include <U8g2lib.h>

// OLED 0.91" 128x32 SSD1306
extern U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C display;
extern float currentTemp;
extern int displayScreen;
extern String deviceIP;
extern unsigned long deviceUptime;
extern int wifiRSSI;

static int lastDisplayScreen = DISPLAY_OFF;

void setDisplayScreen(int screen) {
  int previousScreen = displayScreen;
  displayScreen = screen;
  lastDisplayScreen = previousScreen;
}

void turnOffDisplay() {
  displayScreen = DISPLAY_OFF;
  display.clearBuffer();
  display.sendBuffer();
}

void showTemperatureScreen() {
  display.clearBuffer();
  
  // Средний шрифт для температуры (дисплей 128x32)
  display.setFont(u8g2_font_logisoso22_tn);
  String tempStr = String(currentTemp, 1);
  int tempWidth = display.getUTF8Width(tempStr.c_str());
  int tempX = (128 - tempWidth - 15) / 2;
  display.setCursor(tempX, 26);
  display.print(tempStr);
  
  // Символ градуса
  display.setFont(u8g2_font_ncenB10_tr);
  display.setCursor(tempX + tempWidth + 2, 18);
  display.print("C");
  
  display.sendBuffer();
}

void showInfoScreen() {
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tr);
  
  // Строка 1: Температура и WiFi
  display.setCursor(0, 8);
  display.print(currentTemp, 1);
  display.print("C");
  
  // WiFi справа
  display.setCursor(50, 8);
  if (WiFi.status() == WL_CONNECTED) {
    display.print("WiFi:");
    display.print(wifiRSSI);
    display.print("dB");
  } else if (WiFi.getMode() == WIFI_AP) {
    display.print("AP Mode");
  } else {
    display.print("No WiFi");
  }
  
  // Строка 2: IP адрес
  display.setCursor(0, 18);
  display.print("IP: ");
  display.print(deviceIP);
  
  // Строка 3: Uptime
  unsigned long hours = deviceUptime / 3600;
  unsigned long minutes = (deviceUptime % 3600) / 60;
  display.setCursor(0, 28);
  display.print("Up: ");
  display.print(hours);
  display.print("h ");
  display.print(minutes);
  display.print("m");
  
  display.sendBuffer();
}

void updateDisplay() {
  if (displayScreen == DISPLAY_OFF) {
    return;
  } else if (displayScreen == DISPLAY_TEMP) {
    showTemperatureScreen();
  } else if (displayScreen == DISPLAY_INFO) {
    showInfoScreen();
  }
}

#include "display.h"
#include "config.h"
#include <WiFi.h>
#include <U8g2lib.h>
#include "sensors.h"

// OLED 0.91" 128x32 SSD1306
extern U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C display;
extern float currentTemp;
extern int displayScreen;
extern String deviceIP;
extern unsigned long deviceUptime;
extern int wifiRSSI;
// Forward declaration для SensorConfig
struct SensorConfig {
  String address;
  String name;
  bool enabled;
  float correction;
  String mode;
  bool sendToNetworks;
  bool buzzerEnabled;
  float alertMinTemp;
  float alertMaxTemp;
  bool alertBuzzerEnabled;
  float stabTargetTemp;
  float stabTolerance;
  float stabAlertThreshold;
  unsigned long stabDuration;
  float monitoringThreshold;  // Уставка изменения температуры для отправки в режиме мониторинга (°C)
  bool valid;
};

extern SensorConfig sensorConfigs[];
extern int sensorConfigCount;

static int lastDisplayScreen = DISPLAY_OFF;
static int currentSensorIndex = 0; // Индекс текущего отображаемого термометра

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

void showTemperatureScreen(int sensorIndex) {
  display.clearBuffer();
  
  // Если индекс не указан, используем текущий
  if (sensorIndex < 0) {
    sensorIndex = currentSensorIndex;
  }
  
  // Получаем количество доступных термометров
  int sensorCount = getSensorCount();
  if (sensorCount == 0) {
    display.setFont(u8g2_font_6x10_tr);
    display.setCursor(0, 16);
    display.print("No sensors");
    display.sendBuffer();
    return;
  }
  
  // Ограничиваем индекс
  if (sensorIndex >= sensorCount) {
    sensorIndex = 0;
  }
  if (sensorIndex < 0) {
    sensorIndex = sensorCount - 1;
  }
  
  // Получаем температуру для выбранного термометра
  float temp = getSensorTemperature(sensorIndex);
  if (temp == -127.0) {
    temp = currentTemp; // Fallback на общую температуру
  }
  
  // Применяем коррекцию, если есть настройки
  float correctedTemp = temp;
  String sensorName = "Sensor " + String(sensorIndex + 1);
  for (int i = 0; i < sensorConfigCount; i++) {
    String addressStr = getSensorAddressString(sensorIndex);
    if (sensorConfigs[i].address == addressStr && sensorConfigs[i].enabled) {
      correctedTemp = temp + sensorConfigs[i].correction;
      if (sensorConfigs[i].name.length() > 0) {
        sensorName = sensorConfigs[i].name;
      }
      break;
    }
  }
  
  // Отображаем имя термометра мелким шрифтом (верхняя строка)
  display.setFont(u8g2_font_5x7_tr);
  String nameDisplay = sensorName;
  // Обрезаем имя, если оно слишком длинное
  int maxNameWidth = 120;
  int nameWidth = display.getUTF8Width(nameDisplay.c_str());
  if (nameWidth > maxNameWidth) {
    // Обрезаем имя
    while (nameWidth > maxNameWidth && nameDisplay.length() > 0) {
      nameDisplay = nameDisplay.substring(0, nameDisplay.length() - 1);
      nameWidth = display.getUTF8Width(nameDisplay.c_str());
    }
    nameDisplay += "...";
  }
  display.setCursor(0, 7);
  display.print(nameDisplay);
  
  // Отображаем номер термометра справа (если несколько)
  if (sensorCount > 1) {
    String counter = String(sensorIndex + 1) + "/" + String(sensorCount);
    int counterWidth = display.getUTF8Width(counter.c_str());
    display.setCursor(128 - counterWidth, 7);
    display.print(counter);
  }
  
  // Средний шрифт для температуры (дисплей 128x32)
  display.setFont(u8g2_font_logisoso22_tn);
  String tempStr = String(correctedTemp, 1);
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

int getCurrentSensorIndex() {
  return currentSensorIndex;
}

void setCurrentSensorIndex(int index) {
  int sensorCount = getSensorCount();
  if (sensorCount > 0) {
    if (index < 0) {
      currentSensorIndex = sensorCount - 1;
    } else if (index >= sensorCount) {
      currentSensorIndex = 0;
    } else {
      currentSensorIndex = index;
    }
  } else {
    currentSensorIndex = 0;
  }
}

void nextSensor() {
  int sensorCount = getSensorCount();
  if (sensorCount > 0) {
    currentSensorIndex++;
    if (currentSensorIndex >= sensorCount) {
      currentSensorIndex = 0;
    }
  }
}

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <U8g2lib.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "web_server.h"
#include "tg_bot.h"
#include "sensors.h"
#include "display.h"
#include "time_manager.h"
#include "temperature_history.h"
#include "operation_modes.h"
#include "buzzer.h"
#include "wifi_power.h"

// Forward declarations for MQTT functions
void initMqtt();
void setMqttConfig(const String& server, int port, const String& user, const String& password, const String& topicStatus, const String& topicControl, const String& security);
void updateMqtt();
bool isMqttConfigured();
bool isMqttConnected();
const char* getMqttStatus();

// Объявления для использования в других модулях
extern float currentTemp;
extern unsigned long deviceUptime;
extern String deviceIP;
extern int wifiRSSI;
extern int displayScreen;

// Объявления функций
void sendTemperatureAlert(float temperature);
void sendMetricsToTelegram();

// Инициализация объектов
OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature sensors(&oneWire);
// OLED 0.91" 128x32 SSD1306
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN);

float currentTemp = 0.0;
float lastSentTemp = 0.0;
float targetTemp = 25.0;
unsigned long lastSensorUpdate = 0;
unsigned long lastTelegramUpdate = 0;

// Переменные для дисплея и кнопки
int displayScreen = DISPLAY_OFF;
unsigned long displayTimeout = 0;
unsigned long deviceUptime = 0;
unsigned long deviceStartTime = 0;
unsigned long wifiConnectedSinceMs = 0;
unsigned long wifiConnectedSeconds = 0;
bool wifiWasConnected = false;
String deviceIP = "";
int wifiRSSI = 0;

// Переменные для обработки кнопки
int lastButtonState = HIGH;
unsigned long lastButtonPress = 0;
unsigned long buttonPressStartTime = 0;
bool buttonPressed = false;
int clickCount = 0;
unsigned long lastClickTime = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n\n========================================"));
  Serial.println(F("ESP32 Temperature Monitor Starting..."));
  Serial.println(F("========================================"));
  
  // Инициализация LittleFS (используем раздел "spiffs")
  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
    Serial.println(F("ERROR: Failed to mount LittleFS"));
  } else {
    Serial.println(F("LittleFS mounted OK"));
  }
  
  // Инициализация I2C
  Serial.println(F("Initializing I2C..."));
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  delay(100);
  
  // Инициализация дисплея 0.91" 128x32
  Serial.println(F("Initializing display..."));
  display.begin();
  display.setFont(u8g2_font_6x10_tr);
  display.clearBuffer();
  display.setCursor(0, 12);
  display.print("ESP32 Thermo");
  display.setCursor(0, 26);
  display.print("Starting...");
  display.sendBuffer();

  // Инициализация кнопки
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  lastButtonState = digitalRead(BUTTON_PIN);

  // Инициализация бипера
  initBuzzer();
  
  // Инициализация режимов работы
  initOperationModes();
  
  // Инициализация управления WiFi
  initWiFiPower();
  initMqtt();
  
  deviceStartTime = millis();
  
  // Инициализация датчиков температуры
  Serial.println(F("Initializing temperature sensors..."));
  sensors.begin();
  
  // Инициализация истории
  initTemperatureHistory();
  
  // Загружаем сохраненные настройки WiFi, Telegram и MQTT (если есть)
  String savedSsid;
  String savedPassword;
  String savedTelegramToken;
  String savedTelegramChatId;
  String savedMqttServer;
  int savedMqttPort = 1883;
  String savedMqttUser;
  String savedMqttPassword;
  String savedMqttTopicStatus;
  String savedMqttTopicControl;
  String savedMqttSecurity;
  {
    String settingsJson = getSettings();
    StaticJsonDocument<768> doc;
    DeserializationError error = deserializeJson(doc, settingsJson);
    if (!error && doc.containsKey("wifi")) {
      const char* ssid = doc["wifi"]["ssid"] | "";
      const char* password = doc["wifi"]["password"] | "";
      savedSsid = String(ssid);
      savedPassword = String(password);
    }
    if (!error && doc.containsKey("telegram")) {
      const char* token = doc["telegram"]["bot_token"] | "";
      const char* chatId = doc["telegram"]["chat_id"] | "";
      savedTelegramToken = String(token);
      savedTelegramChatId = String(chatId);
    }
    if (!error && doc.containsKey("mqtt")) {
      savedMqttServer = doc["mqtt"]["server"] | "";
      savedMqttPort = doc["mqtt"]["port"] | 1883;
      savedMqttUser = doc["mqtt"]["user"] | "";
      savedMqttPassword = doc["mqtt"]["password"] | "";
      savedMqttTopicStatus = doc["mqtt"]["topic_status"] | "";
      savedMqttTopicControl = doc["mqtt"]["topic_control"] | "";
      savedMqttSecurity = doc["mqtt"]["security"] | "none";
    }
  }
  {
    String token = savedTelegramToken.length() > 0 ? savedTelegramToken : String(TELEGRAM_BOT_TOKEN);
    String chatId = savedTelegramChatId.length() > 0 ? savedTelegramChatId : String(TELEGRAM_CHAT_ID);
    setTelegramConfig(token, chatId);
  }
  {
    String server = savedMqttServer.length() > 0 ? savedMqttServer : String(MQTT_SERVER);
    int port = savedMqttPort > 0 ? savedMqttPort : MQTT_PORT;
    String user = savedMqttUser.length() > 0 ? savedMqttUser : String(MQTT_USER);
    String password = savedMqttPassword.length() > 0 ? savedMqttPassword : String(MQTT_PASSWORD);
    String topicStatus = savedMqttTopicStatus.length() > 0 ? savedMqttTopicStatus : String(MQTT_TOPIC_STATUS);
    String topicControl = savedMqttTopicControl.length() > 0 ? savedMqttTopicControl : String(MQTT_TOPIC_CONTROL);
    String security = savedMqttSecurity.length() > 0 ? savedMqttSecurity : String("none");
    setMqttConfig(server, port, user, password, topicStatus, topicControl, security);
  }

  // Получаем режим работы
  OperationMode mode = getOperationMode();
  Serial.print(F("Operation mode: "));
  Serial.println(mode);
  
  // Инициализация времени (требует WiFi)
  if (mode != MODE_LOCAL) {
    initTimeManager();
  }
  
  // Настройка WiFi
  Serial.println(F("Initializing WiFi..."));
  
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  if (mode == MODE_LOCAL) {
    // Локальный режим - создаем точку доступа
    Serial.println(F("Starting AP mode..."));
    
    if (startAccessPoint("ESP32_Thermo", "12345678")) {
      deviceIP = WiFi.softAPIP().toString();
    } else {
      deviceIP = "AP Failed";
    }
  } else {
    // Пробуем подключиться к WiFi
    Serial.println(F("Connecting to WiFi..."));
    enableWiFi();
    if (savedSsid.length() > 0) {
      WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    } else {
      WiFi.begin();
    }
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
      deviceIP = WiFi.localIP().toString();
      wifiRSSI = WiFi.RSSI();
      Serial.print(F("WiFi connected! IP: "));
      Serial.println(deviceIP);
    } else {
      // Не удалось подключиться - создаем AP
      Serial.println(F("WiFi failed, starting AP..."));
      
      if (startAccessPoint("ESP32_Thermo", "12345678")) {
        deviceIP = WiFi.softAPIP().toString();
      } else {
        deviceIP = "No connection";
      }
    }
  }
  
  // Запуск веб-сервера и Telegram бота
  Serial.println(F("Starting web server..."));
  startWebServer();
  
  Serial.println(F("Starting Telegram bot..."));
  startTelegramBot();
  
  Serial.println(F("========================================"));
  Serial.println(F("Setup complete!"));
  Serial.print(F("IP: "));
  Serial.println(deviceIP);
  Serial.println(F("========================================"));
  
  // Показываем информацию на дисплее 128x32
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tr);
  display.setCursor(0, 12);
  display.print("Ready!");
  display.setCursor(0, 26);
  display.print("IP:");
  display.print(deviceIP);
  display.sendBuffer();
  
  // Включаем дисплей на 5 секунд
  setDisplayScreen(DISPLAY_INFO);
  displayTimeout = millis() + 5000;
}

void handleButton() {
  int currentButtonState = digitalRead(BUTTON_PIN);
  unsigned long currentTime = millis();

  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonPressStartTime = currentTime;
    buttonPressed = true;
  }

  if (currentButtonState == LOW && buttonPressed) {
    unsigned long pressDuration = currentTime - buttonPressStartTime;
    
    if (pressDuration >= BUTTON_LONG_PRESS_TIME) {
      buttonPressed = false;
      Serial.println(F("Long press - restarting..."));
      WiFi.disconnect(true);
      delay(1000);
      ESP.restart();
    }
  }

  if (currentButtonState == HIGH && lastButtonState == LOW && buttonPressed) {
    buttonPressed = false;
    unsigned long pressDuration = currentTime - buttonPressStartTime;
    
    if (pressDuration < BUTTON_LONG_PRESS_TIME && pressDuration > BUTTON_DEBOUNCE_TIME) {
      unsigned long timeSinceLastClick = currentTime - lastClickTime;
      
      if (timeSinceLastClick < BUTTON_DOUBLE_CLICK_TIME) {
        clickCount = 2;
        Serial.println(F("Double click - display off"));
        setDisplayScreen(DISPLAY_OFF);
        displayTimeout = 0;
        clickCount = 0;
        lastClickTime = 0;
      } else {
        clickCount = 1;
        lastClickTime = currentTime;
        
        if (getOperationMode() == MODE_LOCAL) {
          requestWiFiOn();
        }
        
        if (displayScreen == DISPLAY_OFF) {
          setDisplayScreen(DISPLAY_TEMP);
        } else if (displayScreen == DISPLAY_TEMP) {
          setDisplayScreen(DISPLAY_INFO);
        } else if (displayScreen == DISPLAY_INFO) {
          setDisplayScreen(DISPLAY_TEMP);
        }
        
        displayTimeout = millis() + (DISPLAY_TIMEOUT * 1000);
      }
    }
  }

  lastButtonState = currentButtonState;
  
  if (clickCount == 1 && (currentTime - lastClickTime) > BUTTON_DOUBLE_CLICK_TIME) {
    clickCount = 0;
  }
}

void loop() {
  // Обновление uptime
  deviceUptime = (millis() - deviceStartTime) / 1000;
  
  // Управление питанием WiFi
  updateWiFiPower();
  
  // Обновление информации о WiFi и времени в сети
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected && !wifiWasConnected) {
    wifiConnectedSinceMs = millis();
  }
  if (!wifiConnected) {
    wifiConnectedSinceMs = 0;
    wifiConnectedSeconds = 0;
  } else if (wifiConnectedSinceMs > 0) {
    wifiConnectedSeconds = (millis() - wifiConnectedSinceMs) / 1000;
  }
  wifiWasConnected = wifiConnected;

  if (wifiConnected && isWiFiEnabled() && !isAPMode()) {
    deviceIP = WiFi.localIP().toString();
    wifiRSSI = WiFi.RSSI();
  } else if (isAPMode() || WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    static unsigned long lastAPCheck = 0;
    if (millis() - lastAPCheck > 2000) {
      lastAPCheck = millis();
      IPAddress apIP = WiFi.softAPIP();
      if (apIP.toString() != "0.0.0.0") {
        deviceIP = apIP.toString();
      }
    }
    wifiRSSI = 0;
  } else {
    if (!isWiFiEnabled() && !isAPMode()) {
      deviceIP = "WiFi OFF";
    } else if (!isAPMode()) {
      deviceIP = "Not connected";
    }
    wifiRSSI = 0;
  }
  
  // Обновление бипера
  updateBuzzer();
  
  // Обработка кнопки
  handleButton();
  
  // Автоматическое выключение дисплея
  if (displayScreen != DISPLAY_OFF && displayTimeout > 0 && millis() > displayTimeout) {
    setDisplayScreen(DISPLAY_OFF);
    displayTimeout = 0;
  }
  
  // Обновление времени
  if (isWiFiEnabled()) {
    updateTime();
  }
  
  updateMqtt();
  
  OperationMode mode = getOperationMode();
  
  // Чтение температуры каждые 10 секунд
  if (millis() - lastSensorUpdate > 10000) {
    readTemperature();
    lastSensorUpdate = millis();
    
    addTemperatureRecord(currentTemp);
    
    switch(mode) {
      case MODE_LOCAL:
        break;
        
      case MODE_MONITORING:
        if (abs(currentTemp - lastSentTemp) > 0.1) {
          sendMetricsToTelegram();
          lastSentTemp = currentTemp;
        }
        break;
        
      case MODE_ALERT:
        {
          AlertModeSettings alert = getAlertSettings();
          if (currentTemp <= alert.minTemp || currentTemp >= alert.maxTemp) {
            if (abs(currentTemp - lastSentTemp) > 0.1) {
              sendTemperatureAlert(currentTemp);
              if (alert.buzzerEnabled) {
                buzzerBeep(BUZZER_ALERT);
              }
              lastSentTemp = currentTemp;
            }
          }
        }
        break;
        
      case MODE_STABILIZATION:
        {
          StabilizationModeSettings stab = getStabilizationSettings();
          
          bool stabilized = checkStabilization(currentTemp);
          if (stabilized) {
            buzzerBeep(BUZZER_STABILIZATION);
            sendMetricsToTelegram();
          }
          
          if (checkStabilizationAlert(currentTemp)) {
            buzzerBeep(BUZZER_ALERT);
            sendTemperatureAlert(currentTemp);
          }
        }
        break;
    }
  }
  
  // Обработка Telegram сообщений
  if (WiFi.status() == WL_CONNECTED && millis() - lastTelegramUpdate > 1000) {
    handleTelegramMessages();
    lastTelegramUpdate = millis();
  }
  
  updateDisplay();
}

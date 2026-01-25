#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <U8g2lib.h>
#include <SPIFFS.h>
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
#include "mqtt_client.h"

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
unsigned long lastMqttMetricsUpdate = 0;

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

// Кеш настроек термометров
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
  unsigned long monitoringInterval;  // Интервал отправки в режиме мониторинга (секунды)
  bool valid;
};

#define MAX_SENSORS 10
SensorConfig sensorConfigs[MAX_SENSORS];
int sensorConfigCount = 0;
unsigned long lastSettingsReload = 0;
const unsigned long SETTINGS_RELOAD_INTERVAL = 30000; // Перезагружаем настройки каждые 30 секунд

// Состояния для отслеживания отправки
struct SensorState {
  float lastSentTemp;
  unsigned long stabilizationStartTime;
  bool isStabilized;
};
SensorState sensorStates[MAX_SENSORS];
bool forceReloadSettings = false; // Флаг для принудительной перезагрузки настроек

// Forward declaration
void loadSensorConfigs();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n\n========================================"));
  Serial.println(F("ESP32 Temperature Monitor Starting..."));
  Serial.println(F("========================================"));
  
  // Инициализация SPIFFS
  Serial.println(F("Mounting SPIFFS..."));
  if (!SPIFFS.begin(false)) {
    Serial.println(F("ERROR: Failed to mount SPIFFS, trying to format..."));
    if (!SPIFFS.format()) {
      Serial.println(F("ERROR: Failed to format SPIFFS!"));
      Serial.println(F("Note: Critical settings (WiFi/Telegram) will be loaded from NVS if available"));
    } else {
      Serial.println(F("SPIFFS formatted, restarting..."));
      delay(2000);
      ESP.restart();
    }
  } else {
    Serial.println(F("SPIFFS mounted OK"));
  }
  
  // Preferences (NVS) инициализируется автоматически при первом использовании
  Serial.println(F("Preferences (NVS) ready for critical settings backup"));
  
  // Инициализация истории температуры
  initTemperatureHistory();
  // Загружаем историю из SPIFFS, если она есть
  loadHistoryFromSPIFFS();
  
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
  scanSensors(); // Сканируем все датчики при запуске
  
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
      Serial.print(F("Loaded WiFi SSID: "));
      Serial.println(savedSsid.length() > 0 ? savedSsid : "(empty)");
    } else {
      Serial.println(F("No WiFi settings found in config"));
    }
    if (!error && doc.containsKey("telegram")) {
      const char* token = doc["telegram"]["bot_token"] | "";
      const char* chatId = doc["telegram"]["chat_id"] | "";
      savedTelegramToken = String(token);
      savedTelegramChatId = String(chatId);
    }
    if (!error && doc.containsKey("mqtt")) {
      String server = doc["mqtt"]["server"] | "";
      server.trim();
      // Проверяем валидность адреса сервера
      if (server.length() > 0 && server != "#" && server != "null") {
        savedMqttServer = server;
      } else {
        savedMqttServer = "";
      }
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
    server.trim();
    int port = savedMqttPort > 0 ? savedMqttPort : MQTT_PORT;
    String user = savedMqttUser.length() > 0 ? savedMqttUser : String(MQTT_USER);
    String password = savedMqttPassword.length() > 0 ? savedMqttPassword : String(MQTT_PASSWORD);
    String topicStatus = savedMqttTopicStatus.length() > 0 ? savedMqttTopicStatus : String(MQTT_TOPIC_STATUS);
    String topicControl = savedMqttTopicControl.length() > 0 ? savedMqttTopicControl : String(MQTT_TOPIC_CONTROL);
    String security = savedMqttSecurity.length() > 0 ? savedMqttSecurity : String("none");
    
    // Проверяем, что сервер не является placeholder или пустым
    // Если сервер пустой, "#", "null" или placeholder - отключаем MQTT
    if (server.length() == 0 || 
        server == "#" || 
        server == "null" ||
        server == "mqtt.server.com" ||
        (server.startsWith("mqtt.") && server.endsWith(".com") && server.indexOf("server") != -1)) {
      disableMqtt();
    } else {
      setMqttConfig(server, port, user, password, topicStatus, topicControl, security);
    }
  }

  // Загружаем режим работы из настроек
  OperationMode mode = MODE_LOCAL; // По умолчанию
  {
    String settingsJson = getSettings();
    StaticJsonDocument<256> modeDoc;
    DeserializationError error = deserializeJson(modeDoc, settingsJson);
    if (!error && modeDoc.containsKey("operation_mode")) {
      int modeValue = modeDoc["operation_mode"] | 0;
      mode = (OperationMode)modeValue;
      Serial.print(F("Loaded operation mode: "));
      Serial.println(mode);
    } else {
      Serial.println(F("No saved operation mode"));
    }
  }
  
  // Если есть сохраненный SSID, но режим MODE_LOCAL - переключаем на MODE_MONITORING
  // чтобы устройство подключалось к WiFi, а не создавало точку доступа
  if (mode == MODE_LOCAL && savedSsid.length() > 0) {
    mode = MODE_MONITORING;
    Serial.println(F("SSID found but mode is LOCAL - switching to MONITORING to connect to WiFi"));
  }
  
  // Устанавливаем режим работы
  setOperationMode(mode);
  
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
      Serial.print(F("Connecting to SSID: "));
      Serial.println(savedSsid);
      WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    } else {
      Serial.println(F("No saved SSID, trying to reconnect to last network..."));
      WiFi.begin(); // Пробуем подключиться к последней сети
    }
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) { // Увеличиваем до 30 попыток (15 секунд)
      delay(500);
      yield(); // Даем время другим задачам
      Serial.print(".");
      attempts++;
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
      deviceIP = WiFi.localIP().toString();
      wifiRSSI = WiFi.RSSI();
      
      // Настраиваем DNS серверы явно для предотвращения проблем с DNS lookup
      // Используем публичные DNS серверы Google и Cloudflare
      IPAddress dns1(8, 8, 8, 8);      // Google DNS
      IPAddress dns2(1, 1, 1, 1);      // Cloudflare DNS
      WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
      Serial.println(F("DNS servers configured: 8.8.8.8, 1.1.1.1"));
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
  
  // Инициализация состояний термометров
  for (int i = 0; i < MAX_SENSORS; i++) {
    sensorStates[i].lastSentTemp = 0.0;
    sensorStates[i].stabilizationStartTime = 0;
    sensorStates[i].isStabilized = false;
    sensorConfigs[i].valid = false;
  }
  
  // Загружаем настройки термометров
  loadSensorConfigs();
  
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
          setCurrentSensorIndex(0); // Сбрасываем на первый термометр
        } else if (displayScreen == DISPLAY_TEMP) {
          // Переключаем термометр вместо переключения экрана
          nextSensor();
        } else if (displayScreen == DISPLAY_INFO) {
          setDisplayScreen(DISPLAY_TEMP);
          setCurrentSensorIndex(0); // Сбрасываем на первый термометр
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

// Функция загрузки настроек термометров в кеш
void loadSensorConfigs() {
  sensorConfigCount = 0;
  
  String settingsJson = getSettings();
  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, settingsJson);
  
  if (error || !doc.containsKey("sensors")) {
    Serial.println(F("No sensor settings found or parse error"));
    return;
  }
  
  JsonArray sensorsArray = doc["sensors"];
  int count = sensorsArray.size();
  if (count > MAX_SENSORS) count = MAX_SENSORS;
  
  for (int i = 0; i < count; i++) {
    JsonObject sensor = sensorsArray[i];
    SensorConfig& config = sensorConfigs[sensorConfigCount];
    
    config.address = sensor["address"] | "";
    String nameStr = sensor["name"].as<String>();
    if (nameStr.length() == 0) {
      config.name = "Термометр " + String(sensorConfigCount + 1);
    } else {
      config.name = nameStr;
    }
    config.enabled = sensor["enabled"] | true;
    config.correction = sensor["correction"] | 0.0;
    String modeStr = sensor["mode"] | "monitoring";
    config.mode = modeStr;
    config.sendToNetworks = sensor["sendToNetworks"] | true;
    config.buzzerEnabled = sensor["buzzerEnabled"] | false;
    config.monitoringInterval = sensor["monitoringInterval"] | 5; // По умолчанию 5 секунд
    
    // Настройки оповещения
    if (sensor.containsKey("alertSettings")) {
      JsonObject alert = sensor["alertSettings"];
      config.alertMinTemp = alert["minTemp"] | 10.0;
      config.alertMaxTemp = alert["maxTemp"] | 30.0;
      config.alertBuzzerEnabled = alert["buzzerEnabled"] | true;
    } else {
      config.alertMinTemp = 10.0;
      config.alertMaxTemp = 30.0;
      config.alertBuzzerEnabled = true;
    }
    
    // Настройки стабилизации
    if (sensor.containsKey("stabilizationSettings")) {
      JsonObject stab = sensor["stabilizationSettings"];
      config.stabTargetTemp = stab["targetTemp"] | 25.0;
      config.stabTolerance = stab["tolerance"] | 0.1;
      config.stabAlertThreshold = stab["alertThreshold"] | 0.2;
      config.stabDuration = (stab["duration"] | 10) * 1000; // Конвертируем в миллисекунды
    } else {
      config.stabTargetTemp = 25.0;
      config.stabTolerance = 0.1;
      config.stabAlertThreshold = 0.2;
      config.stabDuration = 10000; // 10 секунд по умолчанию
    }
    
    config.valid = true;
    sensorConfigCount++;
  }
  
  Serial.print(F("Loaded "));
  Serial.print(sensorConfigCount);
  Serial.println(F(" sensor configurations"));
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

  // Приоритет: если WiFi подключен к сети, используем localIP
  if (wifiConnected && isWiFiEnabled()) {
    IPAddress localIP = WiFi.localIP();
    if (localIP.toString() != "0.0.0.0") {
      deviceIP = localIP.toString();
      wifiRSSI = WiFi.RSSI();
    } else {
      // WiFi подключен, но IP еще не получен
      deviceIP = "Получение IP...";
      wifiRSSI = WiFi.RSSI();
    }
  } else if (isAPMode() || WiFi.getMode() == WIFI_AP || (WiFi.getMode() == WIFI_AP_STA && !wifiConnected)) {
    // Режим точки доступа или AP_STA без подключения к WiFi
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
    // WiFi не подключен и не в режиме AP
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
  
  // Отправка метрик аптайма в MQTT каждые 60 секунд
  if (isMqttConnected() && millis() - lastMqttMetricsUpdate > 60000) {
    sendMqttMetrics(deviceUptime, currentTemp, deviceIP, wifiRSSI);
    lastMqttMetricsUpdate = millis();
  }
  
  // Перезагружаем настройки термометров каждые 30 секунд или принудительно после сохранения
  // Но не чаще, чем раз в 5 секунд, чтобы не перегружать систему
  static unsigned long lastReloadCheck = 0;
  if (millis() - lastReloadCheck > 5000) {
    lastReloadCheck = millis();
    if (forceReloadSettings || (millis() - lastSettingsReload > SETTINGS_RELOAD_INTERVAL)) {
      loadSensorConfigs();
      lastSettingsReload = millis();
      forceReloadSettings = false;
    }
  }
  
  // Чтение температуры каждые 10 секунд
  if (millis() - lastSensorUpdate > 10000) {
    readTemperature();
    lastSensorUpdate = millis();
    
    int sensorCount = getSensorCount();
    
    // Обрабатываем каждый термометр
    for (int i = 0; i < sensorCount && i < MAX_SENSORS; i++) {
      uint8_t address[8];
      if (!getSensorAddress(i, address)) {
        continue;
      }
      String addressStr = getSensorAddressString(i);
      
      // Находим настройки по адресу в кеше
      SensorConfig* config = nullptr;
      for (int j = 0; j < sensorConfigCount; j++) {
        if (sensorConfigs[j].valid && sensorConfigs[j].address == addressStr) {
          config = &sensorConfigs[j];
          break;
        }
      }
      
      // Пропускаем термометры без настроек
      if (!config || !config->valid) {
        continue;
      }
      
      // Проверяем, включен ли термометр и отправка в сети
      if (!config->enabled || !config->sendToNetworks) {
        continue;
      }
      
      // Получаем температуру термометра
      float temp = getSensorTemperature(i);
      float correctedTemp = (temp != -127.0) ? (temp + config->correction) : -127.0;
      
      if (correctedTemp == -127.0) {
        continue; // Пропускаем невалидные температуры
      }
      
      // Сохраняем историю температуры для этого термометра
      addTemperatureRecord(correctedTemp, addressStr);
      
      // Обрабатываем режим работы термометра
      if (config->mode == "monitoring") {
        if (abs(correctedTemp - sensorStates[i].lastSentTemp) > 0.1) {
          // Обновляем lastSentTemp для текущего термометра
          sensorStates[i].lastSentTemp = correctedTemp;
          
          // Проверяем, нужно ли отправить метрики (только если WiFi подключен)
          // Используем индивидуальный интервал для каждого термометра
          static unsigned long lastMetricsSend[MAX_SENSORS] = {0};
          unsigned long intervalMs = (config->monitoringInterval > 0) ? (config->monitoringInterval * 1000) : 5000;
          if (WiFi.status() == WL_CONNECTED && (millis() - lastMetricsSend[i] > intervalMs)) {
            // Отправляем метрики для всех термометров одним сообщением
            sendMetricsToTelegram("", -127.0); // Пустое имя означает "отправить все"
            lastMetricsSend[i] = millis();
            
            // Обновляем lastSentTemp для всех термометров, чтобы не отправлять повторно
            // Упрощенная версия - обновляем только те, которые мы обрабатываем
            for (int j = 0; j < sensorCount && j < MAX_SENSORS; j++) {
              if (j == i) continue; // Уже обновили
              uint8_t addr[8];
              if (getSensorAddress(j, addr)) {
                String addrStr = getSensorAddressString(j);
                // Ищем настройки для этого термометра
                for (int k = 0; k < sensorConfigCount; k++) {
                  if (sensorConfigs[k].valid && sensorConfigs[k].address == addrStr) {
                    float temp = getSensorTemperature(j);
                    float corr = (temp != -127.0) ? (temp + sensorConfigs[k].correction) : -127.0;
                    if (corr != -127.0) {
                      sensorStates[j].lastSentTemp = corr;
                    }
                    break; // Нашли, выходим
                  }
                }
              }
              yield(); // Даем время другим задачам
            }
          }
          break; // Обработали, выходим из цикла
        }
      } else if (config->mode == "alert") {
        if (correctedTemp <= config->alertMinTemp || correctedTemp >= config->alertMaxTemp) {
          if (abs(correctedTemp - sensorStates[i].lastSentTemp) > 0.1) {
            String alertType = (correctedTemp >= config->alertMaxTemp) ? "high" : "low";
            sendTemperatureAlert(config->name, correctedTemp, alertType);
            if (config->alertBuzzerEnabled) {
              buzzerBeep(BUZZER_ALERT);
            }
            sensorStates[i].lastSentTemp = correctedTemp;
          }
        }
      } else if (config->mode == "stabilization") {
        float diff = abs(correctedTemp - config->stabTargetTemp);
        
        // Проверка стабилизации
        if (diff <= config->stabTolerance) {
          if (!sensorStates[i].isStabilized) {
            sensorStates[i].isStabilized = true;
            sensorStates[i].stabilizationStartTime = millis();
          }
          
          if (millis() - sensorStates[i].stabilizationStartTime >= config->stabDuration) {
            buzzerBeep(BUZZER_STABILIZATION);
            sensorStates[i].stabilizationStartTime = millis() + 60000; // Не отправляем снова в течение минуты
            
            // Отправляем метрики только если WiFi подключен
            if (WiFi.status() == WL_CONNECTED) {
              sendMetricsToTelegram("", -127.0); // Пустое имя означает "отправить все"
              
              // Обновляем lastSentTemp для всех термометров (упрощенная версия)
              for (int j = 0; j < sensorCount && j < MAX_SENSORS; j++) {
                if (j == i) {
                  sensorStates[j].lastSentTemp = correctedTemp;
                  continue;
                }
                uint8_t addr[8];
                if (getSensorAddress(j, addr)) {
                  String addrStr = getSensorAddressString(j);
                  for (int k = 0; k < sensorConfigCount; k++) {
                    if (sensorConfigs[k].valid && sensorConfigs[k].address == addrStr) {
                      float temp = getSensorTemperature(j);
                      float corr = (temp != -127.0) ? (temp + sensorConfigs[k].correction) : -127.0;
                      if (corr != -127.0) {
                        sensorStates[j].lastSentTemp = corr;
                      }
                      break;
                    }
                  }
                }
                yield(); // Даем время другим задачам
              }
            }
            break; // Обработали, выходим
          }
        } else {
          sensorStates[i].isStabilized = false;
        }
        
        // Проверка тревоги стабилизации
        if (diff > config->stabAlertThreshold) {
          if (abs(correctedTemp - sensorStates[i].lastSentTemp) > 0.1) {
            sendTemperatureAlert(config->name, correctedTemp, "⚠️ Отклонение от целевой температуры!");
            buzzerBeep(BUZZER_ALERT);
            sensorStates[i].lastSentTemp = correctedTemp;
          }
        }
      }
    }
    
    // Старая логика для обратной совместимости (если нет настроек термометров)
    if (sensorConfigCount == 0) {
      // Старая логика для обратной совместимости (один термометр)
      OperationMode mode = getOperationMode();
      String addressStr = (sensorCount > 0) ? getSensorAddressString(0) : "";
      addTemperatureRecord(currentTemp, addressStr);
      
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
  }
  
  // Обработка Telegram сообщений (каждые 5 секунд, чтобы не перегружать API)
  static unsigned long lastTelegramPoll = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastTelegramPoll > 5000) {
    yield(); // Даем время другим задачам перед обработкой Telegram
    handleTelegramMessages();
    lastTelegramPoll = millis();
    yield(); // Даем время после обработки
  }
  
  // Обработка очереди Telegram сообщений (каждую итерацию, но только если есть сообщения)
  // Это должно вызываться часто, чтобы сообщения отправлялись быстро
  // Но не блокируем выполнение, если очередь пуста
  processTelegramQueue();
  
  yield(); // Даем время другим задачам в конце цикла
  
  updateDisplay();
}

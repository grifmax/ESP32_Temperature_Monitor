#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <U8g2lib.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <cmath>  // для fabs()
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

// Конфигурация датчиков (структуры определены в sensor_config.h)
#include "sensor_config.h"

// Глобальные переменные для конфигурации датчиков
SensorConfig sensorConfigs[MAX_SENSORS];
SensorState sensorStates[MAX_SENSORS];
int sensorConfigCount = 0;
bool forceReloadSettings = false;
unsigned long lastSettingsReload = 0;
const unsigned long SETTINGS_RELOAD_INTERVAL = 30000;

// Индекс для быстрого поиска конфигурации по индексу датчика (O(1) вместо O(n))
static int sensorToConfigIndex[MAX_SENSORS];  // -1 означает "нет конфигурации"

// Вспомогательная функция для построения индекса (вызывается после loadSensorConfigs)
static void buildSensorConfigIndex() {
  // Инициализируем индекс значениями -1 (нет конфигурации)
  for (int i = 0; i < MAX_SENSORS; i++) {
    sensorToConfigIndex[i] = -1;
  }

  int sensorCount = getSensorCount();
  for (int i = 0; i < sensorCount && i < MAX_SENSORS; i++) {
    String addressStr = getSensorAddressString(i);
    // Ищем соответствующую конфигурацию (с проверкой границ массива)
    for (int j = 0; j < sensorConfigCount && j < MAX_SENSORS; j++) {
      if (sensorConfigs[j].valid && sensorConfigs[j].address == addressStr) {
        sensorToConfigIndex[i] = j;
        break;
      }
    }
  }
}

// Вспомогательная функция для получения конфигурации по индексу датчика O(1)
static SensorConfig* getConfigForSensor(int sensorIdx) {
  if (sensorIdx < 0 || sensorIdx >= MAX_SENSORS) return nullptr;
  int configIdx = sensorToConfigIndex[sensorIdx];
  if (configIdx < 0 || configIdx >= MAX_SENSORS) return nullptr;
  if (!sensorConfigs[configIdx].valid) return nullptr;
  return &sensorConfigs[configIdx];
}

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
    sensorStates[i].baselineTemp = -127.0;
    sensorStates[i].historyIndex = 0;
    sensorStates[i].historyCount = 0;
    sensorStates[i].alertSent = false;
    sensorStates[i].lastAlertTime = 0;
    for (int j = 0; j < STAB_HISTORY_SIZE; j++) {
      sensorStates[i].tempHistory[j] = -127.0;
      sensorStates[i].timeHistory[j] = 0;
    }
    sensorConfigs[i].valid = false;
  }
  
  // Загружаем настройки термометров
  loadSensorConfigs();
  buildSensorConfigIndex();  // Построение индекса для O(1) поиска

  // Инициализация Watchdog Timer (30 сек таймаут, panic при срабатывании)
  Serial.println(F("Initializing Watchdog Timer..."));
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL); // Добавляем текущую задачу (loop) к WDT
  Serial.println(F("WDT initialized (30s timeout)"));

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
    // Валидация: коррекция в пределах -10..+10 градусов
    config.correction = constrain((float)(sensor["correction"] | 0.0), -10.0f, 10.0f);
    String modeStr = sensor["mode"] | "monitoring";
    config.mode = modeStr;
    config.sendToNetworks = sensor["sendToNetworks"] | true;
    config.buzzerEnabled = sensor["buzzerEnabled"] | false;
    // Валидация: интервал мониторинга 1-3600 секунд (1 сек - 1 час)
    config.monitoringInterval = constrain((int)(sensor["monitoringInterval"] | 5), 1, 3600);

    // Настройки оповещения с валидацией температур (-55..+125°C - диапазон DS18B20)
    if (sensor["alertSettings"].is<JsonObject>()) {
      JsonObject alert = sensor["alertSettings"];
      config.alertMinTemp = constrain((float)(alert["minTemp"] | 10.0), -55.0f, 125.0f);
      config.alertMaxTemp = constrain((float)(alert["maxTemp"] | 30.0), -55.0f, 125.0f);
      config.alertBuzzerEnabled = alert["buzzerEnabled"] | true;
    } else {
      config.alertMinTemp = 10.0;
      config.alertMaxTemp = 30.0;
      config.alertBuzzerEnabled = true;
    }

    // Настройки стабилизации с валидацией
    // tolerance - максимальный разброс температур за duration для признания стабильности
    // alertThreshold - порог резкого скачка от базовой температуры для тревоги
    // duration - время ожидания стабилизации (по умолчанию 10 минут)
    if (sensor["stabilizationSettings"].is<JsonObject>()) {
      JsonObject stab = sensor["stabilizationSettings"];
      config.stabTolerance = constrain((float)(stab["tolerance"] | 0.1), 0.01f, 5.0f);
      config.stabAlertThreshold = constrain((float)(stab["alertThreshold"] | 0.2), 0.05f, 10.0f);
      config.stabBuzzerEnabled = stab["buzzerEnabled"] | true;
      // Валидация: длительность 1-60 минут (60-3600 секунд)
      int durationMin = constrain((int)(stab["duration"] | 10), 1, 60);
      config.stabDuration = durationMin * 60 * 1000UL; // Конвертируем минуты в миллисекунды
    } else {
      config.stabTolerance = 0.1;      // 0.1°C - разброс для стабильности
      config.stabAlertThreshold = 0.2; // 0.2°C - порог резкого скачка
      config.stabBuzzerEnabled = true;
      config.stabDuration = 10 * 60 * 1000UL; // 10 минут по умолчанию
    }
    
    config.valid = true;
    sensorConfigCount++;
  }
  
  Serial.print(F("Loaded "));
  Serial.print(sensorConfigCount);
  Serial.println(F(" sensor configurations"));
}

void loop() {
  // Сбрасываем Watchdog Timer в начале каждой итерации
  esp_task_wdt_reset();

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

  // Обработка отложенной записи в NVS (чтобы не блокировать WiFi при сохранении настроек)
  processPendingNvsSave();

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

  // MQTT теперь обрабатывается в отдельной FreeRTOS задаче (mqtt_client.cpp)
  // Здесь только отправка метрик (не блокирующая операция)

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
      buildSensorConfigIndex();  // Перестраиваем индекс после перезагрузки настроек
      lastSettingsReload = millis();
      forceReloadSettings = false;
    }
  }
  
  // Чтение температуры каждые 10 секунд
  if (millis() - lastSensorUpdate > 10000) {
    readTemperature();
    lastSensorUpdate = millis();

    // Статический массив для отслеживания времени последней отправки метрик
    // (вынесен из цикла для лучшей читаемости и корректности)
    static unsigned long lastMetricsSend[MAX_SENSORS] = {0};

    int sensorCount = getSensorCount();

    // Обрабатываем каждый термометр
    for (int i = 0; i < sensorCount && i < MAX_SENSORS; i++) {
      uint8_t address[8];
      if (!getSensorAddress(i, address)) {
        continue;
      }
      String addressStr = getSensorAddressString(i);

      // Используем O(1) поиск конфигурации через индекс
      SensorConfig* config = getConfigForSensor(i);

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
        if (fabs(correctedTemp - sensorStates[i].lastSentTemp) > 0.1) {
          // Обновляем lastSentTemp для текущего термометра
          sensorStates[i].lastSentTemp = correctedTemp;
          
          // Проверяем, нужно ли отправить метрики (только если WiFi подключен)
          // Используем индивидуальный интервал для каждого термометра
          unsigned long intervalMs = (config->monitoringInterval > 0) ? (config->monitoringInterval * 1000) : 5000;
          if (WiFi.status() == WL_CONNECTED && (millis() - lastMetricsSend[i] > intervalMs)) {
            // Отправляем метрики для всех термометров одним сообщением
            sendMetricsToTelegram("", -127.0); // Пустое имя означает "отправить все"
            lastMetricsSend[i] = millis();
            
            // Обновляем lastSentTemp для всех термометров (O(n) вместо O(n^2))
            for (int j = 0; j < sensorCount && j < MAX_SENSORS; j++) {
              if (j == i) continue; // Уже обновили
              SensorConfig* jConfig = getConfigForSensor(j);
              if (jConfig && jConfig->valid) {
                float jTemp = getSensorTemperature(j);
                float jCorr = (jTemp != -127.0) ? (jTemp + jConfig->correction) : -127.0;
                if (jCorr != -127.0) {
                  sensorStates[j].lastSentTemp = jCorr;
                }
              }
              yield(); // Даем время другим задачам
            }
          }
          break; // Обработали, выходим из цикла
        }
      } else if (config->mode == "alert") {
        if (correctedTemp <= config->alertMinTemp || correctedTemp >= config->alertMaxTemp) {
          if (fabs(correctedTemp - sensorStates[i].lastSentTemp) > 0.1) {
            String alertType = (correctedTemp >= config->alertMaxTemp) ? "high" : "low";
            sendTemperatureAlert(config->name, correctedTemp, alertType);
            if (config->alertBuzzerEnabled) {
              buzzerBeep(BUZZER_ALERT);
            }
            sensorStates[i].lastSentTemp = correctedTemp;
          }
        }
      } else if (config->mode == "stabilization") {
        // === РЕЖИМ СТАБИЛИЗАЦИИ ===
        // Логика: ждём стабилизации температуры, затем отслеживаем РЕЗКИЕ скачки
        // Плавный дрейф (из-за атм. давления и т.д.) - не тревога

        SensorState* state = &sensorStates[i];
        unsigned long now = millis();

        // 1. Добавляем температуру в кольцевой буфер истории
        state->tempHistory[state->historyIndex] = correctedTemp;
        state->timeHistory[state->historyIndex] = now;
        state->historyIndex = (state->historyIndex + 1) % STAB_HISTORY_SIZE;
        if (state->historyCount < STAB_HISTORY_SIZE) {
          state->historyCount++;
        }

        // 2. Анализ стабильности: ищем min/max за период stabDuration
        float minTemp = 999.0, maxTemp = -999.0;
        float sumTemp = 0.0;
        int validCount = 0;

        for (int j = 0; j < state->historyCount; j++) {
          // Проверяем, что запись в пределах duration
          if (now - state->timeHistory[j] <= config->stabDuration) {
            float t = state->tempHistory[j];
            if (t > -100.0) { // Валидная температура
              if (t < minTemp) minTemp = t;
              if (t > maxTemp) maxTemp = t;
              sumTemp += t;
              validCount++;
            }
          }
        }

        // 3. Определяем стабильность: разброс (max-min) <= tolerance
        bool currentlyStable = false;
        float avgTemp = 0.0;

        if (validCount > 0) {
          avgTemp = sumTemp / validCount;
          float spread = maxTemp - minTemp;

          // Стабильна, если разброс в пределах tolerance И прошло достаточно времени
          // Нужно минимум stabDuration/2 данных для надёжного анализа
          unsigned long minDataTime = config->stabDuration / 2;
          unsigned long oldestValidTime = now;
          for (int j = 0; j < state->historyCount; j++) {
            if (now - state->timeHistory[j] <= config->stabDuration && state->tempHistory[j] > -100.0) {
              if (state->timeHistory[j] < oldestValidTime) {
                oldestValidTime = state->timeHistory[j];
              }
            }
          }
          unsigned long dataSpan = now - oldestValidTime;

          currentlyStable = (spread <= config->stabTolerance) && (dataSpan >= minDataTime);
        }

        // 4. Логика переходов состояний
        if (!state->isStabilized) {
          // === Фаза ожидания стабилизации ===
          if (currentlyStable) {
            // Температура стабильна - фиксируем базовую температуру
            state->isStabilized = true;
            state->baselineTemp = avgTemp;
            state->alertSent = false;
            state->stabilizationStartTime = now;

            // Уведомление о стабилизации (один раз)
            Serial.printf("[STAB] %s: стабилизация достигнута, базовая=%.2f°C\n",
                          config->name.c_str(), state->baselineTemp);

            // Короткий сигнал о достижении стабилизации
            buzzerBeep(BUZZER_STABILIZATION);

            // Отправляем уведомление о стабилизации
            if (config->sendToNetworks && WiFi.status() == WL_CONNECTED) {
              String msg = "✅ " + config->name + ": температура стабилизировалась на " +
                          String(state->baselineTemp, 1) + "°C";
              sendTemperatureAlert(config->name, state->baselineTemp, msg);
              state->lastSentTemp = correctedTemp;
            }
          }
        } else {
          // === Фаза отслеживания скачков ===
          float diffFromBaseline = correctedTemp - state->baselineTemp;
          float absDiff = fabs(diffFromBaseline);

          // Проверяем резкий скачок
          if (absDiff >= config->stabAlertThreshold) {
            // Определяем: это резкий скачок или плавный дрейф?
            // Резкий скачок = большое изменение за короткое время
            // Смотрим скорость изменения за последние 30 секунд

            float recentMin = 999.0, recentMax = -999.0;
            for (int j = 0; j < state->historyCount; j++) {
              if (now - state->timeHistory[j] <= 30000) { // последние 30 сек
                float t = state->tempHistory[j];
                if (t > -100.0) {
                  if (t < recentMin) recentMin = t;
                  if (t > recentMax) recentMax = t;
                }
              }
            }

            float recentSpread = (recentMax > -100.0 && recentMin < 100.0) ? (recentMax - recentMin) : 0.0;

            // Резкий скачок: за последние 30 сек изменение >= alertThreshold/2
            bool isSharpJump = (recentSpread >= config->stabAlertThreshold * 0.5f);

            if (isSharpJump) {
              // === ТРЕВОГА: резкий скачок температуры! ===
              // Cooldown 60 секунд между тревогами
              if (!state->alertSent || (now - state->lastAlertTime > 60000)) {
                String direction = (diffFromBaseline > 0) ? "⬆️ РОСТ" : "⬇️ ПАДЕНИЕ";
                String msg = "🚨 " + config->name + ": " + direction + " температуры!\n" +
                            "Было: " + String(state->baselineTemp, 2) + "°C\n" +
                            "Стало: " + String(correctedTemp, 2) + "°C\n" +
                            "Скачок: " + String(diffFromBaseline, 2) + "°C";

                Serial.printf("[STAB] %s: ТРЕВОГА! Скачок %.2f°C (было %.2f, стало %.2f)\n",
                              config->name.c_str(), diffFromBaseline, state->baselineTemp, correctedTemp);

                if (config->stabBuzzerEnabled) {
                  buzzerBeep(BUZZER_ALERT);
                }

                if (config->sendToNetworks && WiFi.status() == WL_CONNECTED) {
                  sendTemperatureAlert(config->name, correctedTemp, msg);
                  state->lastSentTemp = correctedTemp;
                }

                state->alertSent = true;
                state->lastAlertTime = now;
              }
            } else {
              // Плавный дрейф - обновляем базовую температуру
              // (температура медленно изменилась, это нормально)
              Serial.printf("[STAB] %s: плавный дрейф, обновляем базовую %.2f -> %.2f°C\n",
                            config->name.c_str(), state->baselineTemp, avgTemp);
              state->baselineTemp = avgTemp;
              state->alertSent = false; // Сбрасываем флаг тревоги
            }
          } else {
            // Температура в норме - сбрасываем флаг тревоги
            state->alertSent = false;
          }

          // Если температура вышла из стабильного состояния надолго - сбрасываем
          if (!currentlyStable) {
            // Даём 2 минуты на возврат к стабильности
            if (now - state->stabilizationStartTime > 120000) {
              // Переопределяем базовую на текущее среднее
              if (validCount > 0) {
                state->baselineTemp = avgTemp;
                state->stabilizationStartTime = now;
                Serial.printf("[STAB] %s: пересчёт базовой температуры -> %.2f°C\n",
                              config->name.c_str(), state->baselineTemp);
              }
            }
          } else {
            state->stabilizationStartTime = now; // Обновляем время стабильности
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
  
  // Telegram теперь обрабатывается в отдельной FreeRTOS задаче (tg_bot.cpp)
  // Это предотвращает блокировку основного loop при DNS запросах и HTTP операциях

  yield();
  updateDisplay();
}

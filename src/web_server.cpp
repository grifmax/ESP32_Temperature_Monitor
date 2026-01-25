#include "web_server.h"
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
// #include <WiFiManager.h>  // Временно отключено
#include "time_manager.h"
#include "temperature_history.h"
#include "operation_modes.h"
#include "buzzer.h"
#include "tg_bot.h"
#include "mqtt_client.h"
#include "sensors.h"
#include <OneWire.h>
#include <DallasTemperature.h>

extern float currentTemp;
extern DallasTemperature sensors;
extern unsigned long deviceUptime;
extern String deviceIP;
extern int wifiRSSI;
extern int displayScreen;
extern unsigned long wifiConnectedSeconds;
extern DallasTemperature sensors;
// extern WiFiManager wm;  // Временно отключено

AsyncWebServer server(80);

// Файл для хранения настроек
#define SETTINGS_FILE "/settings.json"

// Preferences для надежного хранения критичных настроек
Preferences preferences;

// Флаг для отложенной записи в NVS (чтобы не блокировать WiFi)
volatile bool pendingNvsSave = false;
String pendingNvsData = "";
#define PREF_NAMESPACE "esp32_thermo"
#define PREF_WIFI_SSID "wifi_ssid"
#define PREF_WIFI_PASS "wifi_pass"
#define PREF_TG_TOKEN "tg_token"
#define PREF_TG_CHATID "tg_chatid"
#define PREF_MQTT_SERVER "mqtt_srv"
#define PREF_MQTT_PORT "mqtt_port"
#define PREF_MQTT_USER "mqtt_user"
#define PREF_MQTT_PASS "mqtt_pass"
#define PREF_MQTT_TOPIC_ST "mqtt_topic_st"
#define PREF_MQTT_TOPIC_CT "mqtt_topic_ct"
#define PREF_MQTT_SEC "mqtt_sec"

void startWebServer() {
  // Главная страница
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });
  
  // Статические файлы (без gzip)
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/style.css", "text/css");
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/script.js", "application/javascript");
  });
  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/settings.html", "text/html");
  });
  server.on("/settings.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/settings.js", "application/javascript");
  });
  
  // JSON API endpoint для получения данных
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request){
    // Увеличено для поддержки данных о термометрах
    StaticJsonDocument<2048> doc;
    
    // Получаем актуальный IP адрес
    String currentIP = deviceIP;
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    
    // Если WiFi подключен, приоритет у localIP
    if (wifiConnected) {
      IPAddress localIP = WiFi.localIP();
      if (localIP.toString() != "0.0.0.0") {
        currentIP = localIP.toString();
      }
    } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
      // Если в режиме AP, используем softAPIP
      IPAddress apIP = WiFi.softAPIP();
      if (apIP.toString() != "0.0.0.0") {
        currentIP = apIP.toString();
      }
    }
    
    doc["temperature"] = currentTemp;
    doc["ip"] = currentIP;
    doc["uptime"] = deviceUptime;
    doc["wifi_status"] = wifiConnected ? "connected" : "disconnected";
    doc["wifi_rssi"] = wifiRSSI;
    doc["display_screen"] = displayScreen;
    doc["wifi_connected_seconds"] = wifiConnectedSeconds;
    
    // Форматирование uptime
    unsigned long hours = deviceUptime / 3600;
    unsigned long minutes = (deviceUptime % 3600) / 60;
    unsigned long seconds = deviceUptime % 60;
    char uptimeStr[32];
    snprintf(uptimeStr, sizeof(uptimeStr), "%luh %lum %lus", hours, minutes, seconds);
    doc["uptime_formatted"] = uptimeStr;

    // Форматирование времени в сети
    if (WiFi.status() == WL_CONNECTED && wifiConnectedSeconds > 0) {
      unsigned long wifiHours = wifiConnectedSeconds / 3600;
      unsigned long wifiMinutes = (wifiConnectedSeconds % 3600) / 60;
      unsigned long wifiSeconds = wifiConnectedSeconds % 60;
      char wifiUptimeStr[32];
      snprintf(wifiUptimeStr, sizeof(wifiUptimeStr), "%luh %lum %lus", wifiHours, wifiMinutes, wifiSeconds);
      doc["wifi_connected_formatted"] = wifiUptimeStr;
    } else {
      doc["wifi_connected_formatted"] = "--";
    }
    
    // Добавление времени
    doc["current_time"] = getCurrentTime();
    doc["current_date"] = getCurrentDate();
    doc["unix_time"] = getUnixTime();
    doc["time_synced"] = getUnixTime() > 0;

    // Статусы сервисов
    bool mqttConfigured = isMqttConfigured();
    doc["mqtt"]["configured"] = mqttConfigured;
    doc["mqtt"]["status"] = getMqttStatus();

    bool telegramConfigured = isTelegramConfigured();
    bool telegramInitialized = isTelegramInitialized();
    bool telegramPollOk = isTelegramPollOk();
    unsigned long lastPollMs = getTelegramLastPollMs();
    const char* telegramStatus = "not_configured";
    if (telegramConfigured) {
      if (telegramPollOk && lastPollMs > 0 && (millis() - lastPollMs) < 30000) {
        telegramStatus = "connected";
      } else if (telegramInitialized) {
        telegramStatus = "connecting";
      } else {
        telegramStatus = "not_initialized";
      }
    }
    doc["telegram"]["configured"] = telegramConfigured;
    doc["telegram"]["status"] = telegramStatus;
    if (lastPollMs > 0) {
      doc["telegram"]["last_poll_age"] = (millis() - lastPollMs) / 1000;
    }
    
    // Информация о режиме работы
    OperationMode mode = getOperationMode();
    doc["operation_mode"] = mode;
    const char* modeNames[] = {"local", "monitoring", "alert", "stabilization"};
    doc["operation_mode_name"] = modeNames[mode];
    
    // Дополнительная информация для режима стабилизации
    if (mode == MODE_STABILIZATION) {
      doc["stabilization"]["is_stabilized"] = isStabilized();
      doc["stabilization"]["time"] = getStabilizationTime();
      StabilizationModeSettings stab = getStabilizationSettings();
      doc["stabilization"]["target_temp"] = stab.targetTemp;
      doc["stabilization"]["tolerance"] = stab.tolerance;
    }
    
    // Добавляем информацию о термометрах (автоматическое обнаружение)
    scanSensors();
    int foundCount = getSensorCount();
    JsonArray sensorsArray = doc.createNestedArray("sensors");
    
    // Загружаем настройки из файла
    String settingsJson = getSettings();
    StaticJsonDocument<2048> settingsDoc;
    DeserializationError settingsError = deserializeJson(settingsDoc, settingsJson);
    
    // Создаем карту сохраненных настроек по адресу
    StaticJsonDocument<1024> sensorsMapDoc;
    JsonObject savedSensorsMap = sensorsMapDoc.to<JsonObject>();
    if (!settingsError && settingsDoc.containsKey("sensors") && settingsDoc["sensors"].is<JsonArray>()) {
      JsonArray savedSensors = settingsDoc["sensors"].as<JsonArray>();
      for (JsonObject savedSensor : savedSensors) {
        String savedAddress = savedSensor["address"] | "";
        if (savedAddress.length() > 0) {
          if (!savedSensorsMap.containsKey(savedAddress)) {
            JsonObject sensorMap = savedSensorsMap.createNestedObject(savedAddress);
            sensorMap["name"] = savedSensor["name"] | "";
            sensorMap["enabled"] = savedSensor["enabled"] | true;
            sensorMap["correction"] = savedSensor["correction"] | 0.0;
            sensorMap["mode"] = savedSensor["mode"] | "monitoring";
            sensorMap["monitoringInterval"] = savedSensor["monitoringInterval"] | 5;
            sensorMap["sendToNetworks"] = savedSensor["sendToNetworks"] | true;
            sensorMap["buzzerEnabled"] = savedSensor["buzzerEnabled"] | false;
            if (savedSensor.containsKey("alertSettings")) {
              sensorMap["alertSettings"] = savedSensor["alertSettings"];
            }
            if (savedSensor.containsKey("stabilizationSettings")) {
              sensorMap["stabilizationSettings"] = savedSensor["stabilizationSettings"];
            }
          }
        }
      }
    }
    
    // НЕ вызываем sensors.requestTemperatures() здесь - это блокирует на 750мс!
    // Температура уже обновляется в main.cpp каждые 10 секунд
    // getSensorTemperature() вернёт последнее кешированное значение

    // Добавляем все найденные датчики
    for (int i = 0; i < foundCount; i++) {
      uint8_t address[8];
      if (getSensorAddress(i, address)) {
        String addressStr = getSensorAddressString(i);
        float temp = getSensorTemperature(i);
        
        JsonObject sensor = sensorsArray.createNestedObject();
        sensor["index"] = i;
        sensor["address"] = addressStr;
        
        // Используем сохраненные настройки, если есть
        if (savedSensorsMap.containsKey(addressStr)) {
          JsonObject saved = savedSensorsMap[addressStr];
          String defaultName = "Термометр " + String(i + 1);
          String savedName = saved["name"].as<String>();
          sensor["name"] = (savedName.length() > 0) ? savedName : defaultName;
          sensor["enabled"] = saved["enabled"] | true;
          sensor["correction"] = saved["correction"] | 0.0;
          sensor["mode"] = saved["mode"] | "monitoring";
          sensor["monitoringInterval"] = saved["monitoringInterval"] | 5;
          sensor["sendToNetworks"] = saved["sendToNetworks"] | true;
          sensor["buzzerEnabled"] = saved["buzzerEnabled"] | false;
          
          if (saved.containsKey("alertSettings")) {
            sensor["alertSettings"] = saved["alertSettings"];
          } else {
            sensor["alertSettings"]["minTemp"] = 10.0;
            sensor["alertSettings"]["maxTemp"] = 30.0;
            sensor["alertSettings"]["buzzerEnabled"] = true;
          }
          if (saved.containsKey("stabilizationSettings")) {
            sensor["stabilizationSettings"] = saved["stabilizationSettings"];
          } else {
            sensor["stabilizationSettings"]["targetTemp"] = 25.0;
            sensor["stabilizationSettings"]["tolerance"] = 0.1;
            sensor["stabilizationSettings"]["alertThreshold"] = 0.2;
            sensor["stabilizationSettings"]["duration"] = 10;
          }
        } else {
          // Настройки по умолчанию
          sensor["name"] = "Термометр " + String(i + 1);
          sensor["enabled"] = true;
          sensor["correction"] = 0.0;
          sensor["mode"] = "monitoring";
          sensor["monitoringInterval"] = 5;
          sensor["sendToNetworks"] = true;
          sensor["buzzerEnabled"] = false;
          sensor["alertSettings"]["minTemp"] = 10.0;
          sensor["alertSettings"]["maxTemp"] = 30.0;
          sensor["alertSettings"]["buzzerEnabled"] = true;
          sensor["stabilizationSettings"]["targetTemp"] = 25.0;
          sensor["stabilizationSettings"]["tolerance"] = 0.1;
          sensor["stabilizationSettings"]["alertThreshold"] = 0.2;
          sensor["stabilizationSettings"]["duration"] = 10;
        }
        
        // Текущая температура с учетом коррекции
        float correction = sensor["correction"] | 0.0;
        sensor["currentTemp"] = (temp != -127.0) ? (temp + correction) : -127.0;
        sensor["stabilizationState"] = "tracking";
      }
    }
    
    String response;
    serializeJson(doc, response);
    
    request->send(200, "application/json", response);
  });
  
  // API для получения истории температуры
  server.on("/api/temperature/history", HTTP_GET, [](AsyncWebServerRequest *request){
    String period = request->getParam("period") ? request->getParam("period")->value() : "24h";
    
    unsigned long endTime = getUnixTime();
    unsigned long startTime = endTime;
    
    // Определение периода
    if (period == "1m") {
      startTime = endTime - 60; // 1 минута
    } else if (period == "5m") {
      startTime = endTime - 300; // 5 минут
    } else if (period == "15m") {
      startTime = endTime - 900; // 15 минут
    } else if (period == "30m") {
      startTime = endTime - 1800; // 30 минут
    } else if (period == "1h") {
      startTime = endTime - 3600;
    } else if (period == "6h") {
      startTime = endTime - 21600;
    } else if (period == "24h") {
      startTime = endTime - 86400;
    } else if (period == "7d") {
      startTime = endTime - 604800;
    } else {
      startTime = endTime - 86400; // По умолчанию 24 часа
    }
    
    int count = 0;
    TemperatureRecord* records = getHistoryForPeriod(startTime, endTime, &count);
    
    // Ограничиваем количество записей для экономии памяти
    int maxRecords = count > 100 ? 100 : count; // Максимум 100 записей за раз
    
    StaticJsonDocument<2560> doc;
    JsonArray data = doc.createNestedArray("data");
    
    for (int i = 0; i < maxRecords; i++) {
      // Пропускаем записи с нулевыми или невалидными значениями
      if (records[i].temperature == 0.0 || records[i].temperature == -127.0 || records[i].timestamp == 0) {
        continue;
      }
      
      JsonObject record = data.createNestedObject();
      record["timestamp"] = records[i].timestamp;
      record["temperature"] = records[i].temperature;
      // Добавляем адрес термометра для идентификации
      if (records[i].sensorAddress.length() > 0) {
        record["sensor_address"] = records[i].sensorAddress;
        record["sensor_id"] = records[i].sensorAddress; // Для совместимости
      }
      yield(); // Предотвращаем watchdog reset
    }
    
    doc["count"] = maxRecords;
    doc["period"] = period;
    
    String response;
    serializeJson(doc, response);
    
    request->send(200, "application/json", response);
  });
  
  // API для запуска сканирования Wi-Fi сетей (асинхронное)
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("WiFi scan requested..."));

    // Убеждаемся, что WiFi в режиме, позволяющем сканировать
    if (WiFi.getMode() == WIFI_AP) {
      WiFi.mode(WIFI_AP_STA);
      delay(100);
    } else if (WiFi.getMode() == WIFI_OFF) {
      WiFi.mode(WIFI_STA);
      delay(100);
    }
    
    // Проверяем, есть ли уже результаты сканирования
    int n = WiFi.scanComplete();
    
    if (n == WIFI_SCAN_FAILED) {
      // Сканирование не запущено - запускаем асинхронное сканирование
      Serial.println(F("Starting async WiFi scan..."));
      WiFi.scanNetworks(true); // true = асинхронный режим
      request->send(200, "application/json", "{\"status\":\"scanning\",\"networks\":[]}");
      return;
    }
    
    if (n == WIFI_SCAN_RUNNING) {
      // Сканирование еще выполняется
      request->send(200, "application/json", "{\"status\":\"scanning\",\"networks\":[]}");
      return;
    }
    
    // Сканирование завершено, есть результаты
    Serial.printf("WiFi scan found %d networks\n", n);
    
    // Ограничиваем количество сетей для экономии памяти (макс 15 сетей)
    int maxNetworks = n > 15 ? 15 : n;
    
    StaticJsonDocument<1536> doc;
    doc["status"] = "complete";
    JsonArray networks = doc.createNestedArray("networks");
    
    for (int i = 0; i < maxNetworks; i++) {
      JsonObject network = networks.createNestedObject();
      network["ssid"] = WiFi.SSID(i);
      network["rssi"] = WiFi.RSSI(i);
      network["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "encrypted";
      network["channel"] = WiFi.channel(i);
    }
    
    doc["count"] = maxNetworks;
    
    String response;
    serializeJson(doc, response);
    
    // Очищаем результаты сканирования для следующего запроса
    WiFi.scanDelete();
    
    request->send(200, "application/json", response);
  });
  
  // API для получения текущих настроек
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    String settings = getSettings();
    request->send(200, "application/json", settings);
  });
  
  // API для сохранения настроек
  static String settingsRequestBody = "";
  server.on("/api/settings", HTTP_POST, 
    [](AsyncWebServerRequest *request){
      // Начало запроса - очищаем буфер
      settingsRequestBody = "";
    }, 
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      // Добавляем данные к буферу
      settingsRequestBody += String((char*)data).substring(0, len);
      
      // Если это последний фрагмент, обрабатываем
      if (index + len >= total) {
        yield(); // Даем время другим задачам
        
        if (saveSettings(settingsRequestBody)) {
          request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
          request->send(500, "application/json", "{\"status\":\"error\"}");
        }
        
        // Очищаем буфер после обработки
        settingsRequestBody = "";
      }
    });
  
  // API для получения списка термометров
  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<4096> doc;
    JsonArray sensorsArray = doc.createNestedArray("sensors");
    
    // Сканируем все датчики на шине OneWire
    scanSensors();
    int foundCount = getSensorCount();
    
    // Загружаем настройки из файла
    String settingsJson = getSettings();
    StaticJsonDocument<2048> settingsDoc;
    DeserializationError error = deserializeJson(settingsDoc, settingsJson);
    
    // Создаем карту сохраненных настроек по адресу
    StaticJsonDocument<1024> sensorsMapDoc;
    JsonObject savedSensorsMap = sensorsMapDoc.to<JsonObject>();
    if (!error && settingsDoc.containsKey("sensors") && settingsDoc["sensors"].is<JsonArray>()) {
      JsonArray savedSensors = settingsDoc["sensors"].as<JsonArray>();
      for (JsonObject savedSensor : savedSensors) {
        String savedAddress = savedSensor["address"] | "";
        if (savedAddress.length() > 0) {
          // Создаем объект для этого адреса
          JsonObject sensorMap = savedSensorsMap.createNestedObject(savedAddress);
          sensorMap["name"] = savedSensor["name"] | "";
          sensorMap["enabled"] = savedSensor["enabled"] | true;
          sensorMap["correction"] = savedSensor["correction"] | 0.0;
          String modeStr = savedSensor["mode"] | "monitoring";
          sensorMap["mode"] = modeStr;
          sensorMap["monitoringInterval"] = savedSensor["monitoringInterval"] | 5;
          sensorMap["sendToNetworks"] = savedSensor["sendToNetworks"] | true;
          sensorMap["buzzerEnabled"] = savedSensor["buzzerEnabled"] | false;
          if (savedSensor.containsKey("alertSettings")) {
            sensorMap["alertSettings"] = savedSensor["alertSettings"];
          }
          if (savedSensor.containsKey("stabilizationSettings")) {
            sensorMap["stabilizationSettings"] = savedSensor["stabilizationSettings"];
          }
        }
      }
    }
    
    // Добавляем все найденные датчики
    // НЕ вызываем sensors.requestTemperatures() - температура обновляется в main loop

    for (int i = 0; i < foundCount; i++) {
      uint8_t address[8];
      if (getSensorAddress(i, address)) {
        String addressStr = getSensorAddressString(i);
        float temp = getSensorTemperature(i);
        
        JsonObject sensor = sensorsArray.createNestedObject();
        sensor["index"] = i;
        sensor["address"] = addressStr;
        
        // Используем сохраненные настройки, если есть
        if (savedSensorsMap.containsKey(addressStr)) {
          JsonObject saved = savedSensorsMap[addressStr];
          String defaultName = "Термометр " + String(i + 1);
          String savedName = saved["name"].as<String>();
          sensor["name"] = (savedName.length() > 0) ? savedName : defaultName;
          sensor["enabled"] = saved["enabled"] | true;
          sensor["correction"] = saved["correction"] | 0.0;
          sensor["mode"] = saved["mode"] | "monitoring";
          sensor["monitoringInterval"] = saved["monitoringInterval"] | 5;
          sensor["sendToNetworks"] = saved["sendToNetworks"] | true;
          sensor["buzzerEnabled"] = saved["buzzerEnabled"] | false;
          
          if (saved.containsKey("alertSettings")) {
            sensor["alertSettings"] = saved["alertSettings"];
          } else {
            sensor["alertSettings"]["minTemp"] = 10.0;
            sensor["alertSettings"]["maxTemp"] = 30.0;
            sensor["alertSettings"]["buzzerEnabled"] = true;
          }
          
          if (saved.containsKey("stabilizationSettings")) {
            sensor["stabilizationSettings"] = saved["stabilizationSettings"];
          } else {
            sensor["stabilizationSettings"]["targetTemp"] = 25.0;
            sensor["stabilizationSettings"]["tolerance"] = 0.1;
            sensor["stabilizationSettings"]["alertThreshold"] = 0.2;
            sensor["stabilizationSettings"]["duration"] = 10;
          }
        } else {
          // Настройки по умолчанию для нового датчика
          sensor["name"] = "Термометр " + String(i + 1);
          sensor["enabled"] = true;
          sensor["correction"] = 0.0;
          sensor["mode"] = "monitoring";
          sensor["monitoringInterval"] = 5;
          sensor["sendToNetworks"] = true;
          sensor["buzzerEnabled"] = false;
          sensor["alertSettings"]["minTemp"] = 10.0;
          sensor["alertSettings"]["maxTemp"] = 30.0;
          sensor["alertSettings"]["buzzerEnabled"] = true;
          sensor["stabilizationSettings"]["targetTemp"] = 25.0;
          sensor["stabilizationSettings"]["tolerance"] = 0.1;
          sensor["stabilizationSettings"]["alertThreshold"] = 0.2;
          sensor["stabilizationSettings"]["duration"] = 10;
        }
        
        // Текущая температура с учетом коррекции
        float correction = sensor["correction"] | 0.0;
        sensor["currentTemp"] = (temp != -127.0) ? (temp + correction) : -127.0;
        sensor["stabilizationState"] = "tracking";
      }
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  // API для сохранения списка термометров
  static String sensorsRequestBody = "";
  server.on("/api/sensors", HTTP_POST, 
    [](AsyncWebServerRequest *request){
      sensorsRequestBody = "";
    }, 
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      sensorsRequestBody += String((char*)data).substring(0, len);
      
      if (index + len >= total) {
        yield(); // Даем время другим задачам
        
        StaticJsonDocument<8192> doc; // Увеличиваем размер для всех настроек термометров
        DeserializationError error = deserializeJson(doc, sensorsRequestBody);
        
        if (error) {
          Serial.print(F("ERROR: Failed to parse sensors JSON: "));
          Serial.println(error.c_str());
          Serial.print(F("JSON length: "));
          Serial.println(sensorsRequestBody.length());
          request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
          sensorsRequestBody = "";
          return;
        }
        
        if (!doc.containsKey("sensors")) {
          Serial.println(F("ERROR: Missing 'sensors' key in JSON"));
          request->send(400, "application/json", "{\"error\":\"Missing 'sensors' key\"}");
          sensorsRequestBody = "";
          return;
        }
        
        // Загружаем существующие настройки
        String settingsJson = getSettings();
        StaticJsonDocument<8192> settingsDoc; // Увеличиваем размер
        DeserializationError settingsError = deserializeJson(settingsDoc, settingsJson);
        
        if (settingsError) {
          Serial.print(F("ERROR: Failed to parse existing settings: "));
          Serial.println(settingsError.c_str());
          request->send(500, "application/json", "{\"error\":\"Failed to load existing settings\"}");
          sensorsRequestBody = "";
          return;
        }
        
        // Сохраняем датчики в настройки
        settingsDoc["sensors"] = doc["sensors"];
        
        yield(); // Даем время перед сериализацией
        
        // Сохраняем в файл
        String mergedJson;
        serializeJson(settingsDoc, mergedJson);
        
        Serial.print(F("Saving sensors, JSON size: "));
        Serial.println(mergedJson.length());
        
        if (saveSettings(mergedJson)) {
          // Устанавливаем флаг для принудительной перезагрузки настроек в main.cpp
          extern bool forceReloadSettings;
          forceReloadSettings = true;
          request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
          Serial.println(F("ERROR: saveSettings returned false"));
          request->send(500, "application/json", "{\"error\":\"Failed to save settings\"}");
        }
        
        sensorsRequestBody = "";
      }
    });
  
  // API для получения настроек конкретного термометра
  server.on("^/api/sensor/([0-9]+)$", HTTP_GET, [](AsyncWebServerRequest *request){
    String sensorId = request->pathArg(0);
    int id = sensorId.toInt();
    
    StaticJsonDocument<384> doc;
    doc["id"] = id;
    doc["name"] = "Термометр " + sensorId;
    doc["enabled"] = true;
    doc["correction"] = 0.0;
    doc["mode"] = "monitoring";
    doc["sendToNetworks"] = true;
    doc["buzzerEnabled"] = false;
    doc["alertSettings"]["minTemp"] = 10.0;
    doc["alertSettings"]["maxTemp"] = 30.0;
    doc["alertSettings"]["buzzerEnabled"] = true;
    doc["stabilizationSettings"]["targetTemp"] = 25.0;
    doc["stabilizationSettings"]["tolerance"] = 0.1;
    doc["stabilizationSettings"]["alertThreshold"] = 0.2;
    doc["stabilizationSettings"]["duration"] = 10;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  // API для сохранения настроек конкретного термометра
  static String sensorRequestBody = "";
  server.on("^/api/sensor/([0-9]+)$", HTTP_POST, 
    [](AsyncWebServerRequest *request){
      sensorRequestBody = "";
    }, 
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      sensorRequestBody += String((char*)data).substring(0, len);
      
      if (index + len >= total) {
        String sensorId = request->pathArg(0);
        int id = sensorId.toInt();
        
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, sensorRequestBody);
        
        if (!error) {
          // TODO: Сохранять настройки термометра в файл настроек
          request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
          request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        }
        
        sensorRequestBody = "";
      }
    });
  
  // API для получения режима работы
  server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<256> doc;
    OperationMode mode = getOperationMode();
    doc["mode"] = mode;
    
    if (mode == MODE_ALERT) {
      AlertModeSettings alert = getAlertSettings();
      doc["alert"]["min_temp"] = alert.minTemp;
      doc["alert"]["max_temp"] = alert.maxTemp;
      doc["alert"]["buzzer_enabled"] = alert.buzzerEnabled;
    } else if (mode == MODE_STABILIZATION) {
      StabilizationModeSettings stab = getStabilizationSettings();
      doc["stabilization"]["target_temp"] = stab.targetTemp;
      doc["stabilization"]["tolerance"] = stab.tolerance;
      doc["stabilization"]["alert_threshold"] = stab.alertThreshold;
      doc["stabilization"]["duration"] = stab.duration;
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  // API для сохранения режима работы
  static String modeRequestBody = "";
  server.on("/api/mode", HTTP_POST, 
    [](AsyncWebServerRequest *request){
      modeRequestBody = "";
    }, 
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      modeRequestBody += String((char*)data).substring(0, len);
      
      if (index + len >= total) {
        yield(); // Даем время другим задачам
        
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, modeRequestBody);
        
        if (!error && doc.containsKey("mode")) {
          int mode = doc["mode"];
          setOperationMode((OperationMode)mode);
          yield();
          
          if (mode == MODE_ALERT && doc.containsKey("alert")) {
            float minTemp = doc["alert"]["min_temp"] | 10.0;
            float maxTemp = doc["alert"]["max_temp"] | 30.0;
            bool buzzerEnabled = doc["alert"]["buzzer_enabled"] | true;
            setAlertSettings(minTemp, maxTemp, buzzerEnabled);
            yield();
          } else if (mode == MODE_STABILIZATION && doc.containsKey("stabilization")) {
            float targetTemp = doc["stabilization"]["target_temp"] | 25.0;
            float tolerance = doc["stabilization"]["tolerance"] | 0.1;
            float alertThreshold = doc["stabilization"]["alert_threshold"] | 0.2;
            unsigned long duration = doc["stabilization"]["duration"] | 600;
            setStabilizationSettings(targetTemp, tolerance, alertThreshold, duration);
            yield();
          }
          
          request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
          request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        }
        
        modeRequestBody = "";
      }
    });
  
  // API для подключения к Wi-Fi
  static String wifiConnectRequestBody = "";
  server.on("/api/wifi/connect", HTTP_POST, 
    [](AsyncWebServerRequest *request){
      wifiConnectRequestBody = "";
    }, 
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      wifiConnectRequestBody += String((char*)data).substring(0, len);
      
      if (index + len >= total) {
        yield(); // Даем время другим задачам
        
        StaticJsonDocument<256> doc;
        deserializeJson(doc, wifiConnectRequestBody);
        
        String ssid = doc["ssid"] | "";
        String password = doc["password"] | "";
        
        if (ssid.length() > 0) {
          WiFi.disconnect(true);
          WiFi.mode(WIFI_STA);
          WiFi.setAutoReconnect(true);
          WiFi.begin(ssid.c_str(), password.c_str());
          yield(); // Даем время после операций WiFi
          request->send(200, "application/json", "{\"status\":\"connecting\"}");
        } else {
          request->send(400, "application/json", "{\"status\":\"invalid\"}");
        }
        
        wifiConnectRequestBody = "";
      }
    });
  
  // API для отправки тестового сообщения в Telegram
  server.on("/api/telegram/test", HTTP_POST, [](AsyncWebServerRequest *request){
    yield(); // Даем время другим задачам
    
    // Проверяем подключение WiFi
    if (WiFi.status() != WL_CONNECTED) {
      request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"WiFi not connected\"}");
      return;
    }
    
    yield();
    bool success = sendTelegramTestMessage();
    yield(); // Даем время после отправки
    
    if (success) {
      request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Test message sent\"}");
    } else {
      request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to send test message\"}");
    }
  });
  
  // API для отправки тестового сообщения в MQTT
  server.on("/api/mqtt/test", HTTP_POST, [](AsyncWebServerRequest *request){
    yield();
    bool success = sendMqttTestMessage();
    if (success) {
      request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Test message sent\"}");
    } else {
      request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to send test message\"}");
    }
  });
  
  // API для принудительного отключения MQTT
  server.on("/api/mqtt/disable", HTTP_POST, [](AsyncWebServerRequest *request){
    yield();
    disableMqtt();
    request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"MQTT disabled\"}");
  });
  
  // Обработчик для OPTIONS запросов (CORS preflight)
  server.onNotFound([](AsyncWebServerRequest *request){
    if (request->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse *response = request->beginResponse(200);
      response->addHeader("Access-Control-Allow-Origin", "*");
      response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      response->addHeader("Access-Control-Allow-Headers", "Content-Type");
      request->send(response);
    } else if (request->url() == "/favicon.ico") {
      // Отправляем пустой ответ для favicon.ico
      request->send(204);
    } else {
      // 404 для всех остальных необработанных запросов
      request->send(404, "text/plain", "Not Found");
    }
  });
  
  server.begin();
  Serial.println(F("Web server started"));
}

// Функция получения настроек из файла с резервным чтением из Preferences
String getSettings() {
  StaticJsonDocument<768> doc;
  
  // Сначала пытаемся загрузить из SPIFFS
  File file = SPIFFS.open(SETTINGS_FILE, "r");
  if (file) {
    String content = file.readString();
    file.close();
    
    DeserializationError error = deserializeJson(doc, content);
    if (!error) {
      Serial.println(F("Settings loaded from SPIFFS"));
    } else {
      Serial.println(F("Failed to parse SPIFFS settings, trying Preferences"));
    }
  } else {
    Serial.println(F("SPIFFS settings file not found, trying Preferences"));
  }
  
  // Проверяем Preferences для критичных настроек (WiFi, Telegram, MQTT)
  // Используем их если в SPIFFS нет данных или они пустые
  // Используем прямое чтение с значениями по умолчанию - это не вызовет ошибок,
  // если ключи не существуют (просто вернутся пустые строки/нули)
  preferences.begin(PREF_NAMESPACE, true); // read-only mode
  
  String wifiSsid = preferences.getString(PREF_WIFI_SSID, "");
  String wifiPass = preferences.getString(PREF_WIFI_PASS, "");
  String tgToken = preferences.getString(PREF_TG_TOKEN, "");
  String tgChatId = preferences.getString(PREF_TG_CHATID, "");
  String mqttServer = preferences.getString(PREF_MQTT_SERVER, "");
  int mqttPort = preferences.getInt(PREF_MQTT_PORT, 0);
  String mqttUser = preferences.getString(PREF_MQTT_USER, "");
  String mqttPass = preferences.getString(PREF_MQTT_PASS, "");
  String mqttTopicSt = preferences.getString(PREF_MQTT_TOPIC_ST, "");
  String mqttTopicCt = preferences.getString(PREF_MQTT_TOPIC_CT, "");
  String mqttSec = preferences.getString(PREF_MQTT_SEC, "");
  
  preferences.end();
  
  // Если в Preferences есть данные, используем их (приоритет над SPIFFS для критичных настроек)
  bool usePrefs = false;
  if (wifiSsid.length() > 0) {
    String spiffsSsid = doc.containsKey("wifi") ? doc["wifi"]["ssid"].as<String>() : "";
    if (spiffsSsid.length() == 0) {
      usePrefs = true;
      Serial.println(F("WiFi SSID from Preferences (SPIFFS empty)"));
    }
  }
  if (tgToken.length() > 0) {
    String spiffsToken = doc.containsKey("telegram") ? doc["telegram"]["bot_token"].as<String>() : "";
    if (spiffsToken.length() == 0) {
      usePrefs = true;
      Serial.println(F("Telegram token from Preferences (SPIFFS empty)"));
    }
  }
  if (mqttServer.length() > 0) {
    String spiffsMqtt = doc.containsKey("mqtt") ? doc["mqtt"]["server"].as<String>() : "";
    if (spiffsMqtt.length() == 0) {
      usePrefs = true;
      Serial.println(F("MQTT server from Preferences (SPIFFS empty)"));
    }
  }
  
  if (usePrefs && (wifiSsid.length() > 0 || tgToken.length() > 0 || mqttServer.length() > 0)) {
    Serial.println(F("Loading critical settings from Preferences (NVS)"));
    if (wifiSsid.length() > 0) {
      doc["wifi"]["ssid"] = wifiSsid;
      doc["wifi"]["password"] = wifiPass;
    }
    if (tgToken.length() > 0) {
      doc["telegram"]["bot_token"] = tgToken;
      doc["telegram"]["chat_id"] = tgChatId;
    }
    if (mqttServer.length() > 0) {
      doc["mqtt"]["server"] = mqttServer;
      if (mqttPort > 0) {
        doc["mqtt"]["port"] = mqttPort;
      }
      doc["mqtt"]["user"] = mqttUser;
      doc["mqtt"]["password"] = mqttPass;
      if (mqttTopicSt.length() > 0) {
        doc["mqtt"]["topic_status"] = mqttTopicSt;
      }
      if (mqttTopicCt.length() > 0) {
        doc["mqtt"]["topic_control"] = mqttTopicCt;
      }
      if (mqttSec.length() > 0) {
        doc["mqtt"]["security"] = mqttSec;
      }
    }
  }
  
  // Устанавливаем значения по умолчанию, если их нет
  if (!doc.containsKey("wifi")) {
    doc["wifi"]["ssid"] = "";
    doc["wifi"]["password"] = "";
  }
  if (!doc.containsKey("mqtt")) {
    doc["mqtt"]["server"] = "";
    doc["mqtt"]["port"] = 1883;
    doc["mqtt"]["user"] = "";
    doc["mqtt"]["password"] = "";
    doc["mqtt"]["topic_status"] = "home/thermo/status";
    doc["mqtt"]["topic_control"] = "home/thermo/control";
    doc["mqtt"]["security"] = "none";
  }
  if (!doc.containsKey("telegram")) {
    doc["telegram"]["bot_token"] = "";
    doc["telegram"]["chat_id"] = "";
  }
  if (!doc.containsKey("temperature")) {
    doc["temperature"]["high_threshold"] = 30.0;
    doc["temperature"]["low_threshold"] = 10.0;
  }
  if (!doc.containsKey("timezone")) {
    doc["timezone"]["offset"] = 3; // UTC+3 по умолчанию
  }
  if (!doc.containsKey("operation_mode")) {
    doc["operation_mode"] = 0; // MODE_LOCAL по умолчанию
  }
  if (!doc.containsKey("alert")) {
    doc["alert"]["min_temp"] = 10.0;
    doc["alert"]["max_temp"] = 30.0;
    doc["alert"]["buzzer_enabled"] = true;
  }
  if (!doc.containsKey("stabilization")) {
    doc["stabilization"]["target_temp"] = 25.0;
    doc["stabilization"]["tolerance"] = 0.1;
    doc["stabilization"]["alert_threshold"] = 0.2;
    doc["stabilization"]["duration"] = 600;
  }
  
  // Применяем часовой пояс при загрузке
  if (doc["timezone"].containsKey("offset")) {
    int offset = doc["timezone"]["offset"];
    setTimezone(offset);
  }
  
  String result;
  serializeJson(doc, result);
  return result;
}

// Функция сохранения настроек в файл
bool saveSettings(String json) {
  yield(); // Даем время другим задачам перед началом обработки
  
  // Загружаем существующие настройки из файла
  // Увеличиваем размер документа для обработки всех настроек термометров
  StaticJsonDocument<8192> existingDoc;
  String existingContent = "";
  File existingFile = SPIFFS.open(SETTINGS_FILE, "r");
  if (existingFile) {
    existingContent = existingFile.readString();
    existingFile.close();
    if (existingContent.length() > 0) {
      DeserializationError existingParseError = deserializeJson(existingDoc, existingContent);
      if (existingParseError) {
        Serial.print(F("Warning: Failed to parse existing settings: "));
        Serial.println(existingParseError.c_str());
      }
    }
  }
  
  yield(); // Даем время после чтения файла
  
  // Парсим новые настройки из запроса
  // Увеличиваем размер для обработки всех настроек термометров
  StaticJsonDocument<8192> newDoc;
  DeserializationError error = deserializeJson(newDoc, json);
  if (error) {
    Serial.print(F("Failed to parse settings JSON: "));
    Serial.println(error.c_str());
    Serial.print(F("JSON length: "));
    Serial.println(json.length());
    return false;
  }
  
  yield(); // Даем время после парсинга JSON
  
  // Объединяем настройки: сначала копируем существующие, затем перезаписываем новыми
  StaticJsonDocument<8192> mergedDoc;
  
  // Копируем весь существующий документ через сериализацию/десериализацию для глубокого копирования
  if (existingContent.length() > 0 && existingContent != "null") {
    DeserializationError existingError = deserializeJson(mergedDoc, existingContent);
    if (existingError) {
      // Если ошибка парсинга существующего файла, начинаем с пустого документа
      Serial.println(F("Failed to parse existing settings, starting fresh"));
    }
  }
  
  // Если файл был пустой или не удалось распарсить, инициализируем значениями по умолчанию
  if (!mergedDoc.containsKey("wifi")) {
    mergedDoc["wifi"]["ssid"] = "";
    mergedDoc["wifi"]["password"] = "";
  }
  if (!mergedDoc.containsKey("mqtt")) {
    mergedDoc["mqtt"]["server"] = "";
    mergedDoc["mqtt"]["port"] = 1883;
    mergedDoc["mqtt"]["user"] = "";
    mergedDoc["mqtt"]["password"] = "";
    mergedDoc["mqtt"]["topic_status"] = "home/thermo/status";
    mergedDoc["mqtt"]["topic_control"] = "home/thermo/control";
    mergedDoc["mqtt"]["security"] = "none";
  }
  if (!mergedDoc.containsKey("telegram")) {
    mergedDoc["telegram"]["bot_token"] = "";
    mergedDoc["telegram"]["chat_id"] = "";
  }
  if (!mergedDoc.containsKey("temperature")) {
    mergedDoc["temperature"]["high_threshold"] = 30.0;
    mergedDoc["temperature"]["low_threshold"] = 10.0;
  }
  if (!mergedDoc.containsKey("timezone")) {
    mergedDoc["timezone"]["offset"] = 3;
  }
  if (!mergedDoc.containsKey("operation_mode")) {
    mergedDoc["operation_mode"] = 0;
  }
  if (!mergedDoc.containsKey("alert")) {
    mergedDoc["alert"]["min_temp"] = 10.0;
    mergedDoc["alert"]["max_temp"] = 30.0;
    mergedDoc["alert"]["buzzer_enabled"] = true;
  }
  if (!mergedDoc.containsKey("stabilization")) {
    mergedDoc["stabilization"]["target_temp"] = 25.0;
    mergedDoc["stabilization"]["tolerance"] = 0.1;
    mergedDoc["stabilization"]["alert_threshold"] = 0.2;
    mergedDoc["stabilization"]["duration"] = 600;
  }
  
  // Перезаписываем только те секции, которые есть в новом запросе (частичное обновление)
  if (newDoc.containsKey("wifi")) {
    // Обновляем только поля WiFi, которые есть в новом запросе
    if (newDoc["wifi"].containsKey("ssid")) {
      mergedDoc["wifi"]["ssid"] = newDoc["wifi"]["ssid"];
    }
    if (newDoc["wifi"].containsKey("password")) {
      mergedDoc["wifi"]["password"] = newDoc["wifi"]["password"];
    }
  }
  
  if (newDoc.containsKey("telegram")) {
    // Обновляем только поля Telegram, которые есть в новом запросе
    if (newDoc["telegram"].containsKey("bot_token")) {
      mergedDoc["telegram"]["bot_token"] = newDoc["telegram"]["bot_token"];
    }
    if (newDoc["telegram"].containsKey("chat_id")) {
      mergedDoc["telegram"]["chat_id"] = newDoc["telegram"]["chat_id"];
    }
  }
  
  if (newDoc.containsKey("mqtt")) {
    // Обновляем только поля MQTT, которые есть в новом запросе
    if (newDoc["mqtt"].containsKey("server")) {
      String server = newDoc["mqtt"]["server"] | "";
      server.trim();
      if (server == "#" || server == "null" || server.length() == 0) {
        mergedDoc["mqtt"]["server"] = "";
      } else {
        mergedDoc["mqtt"]["server"] = server;
      }
    }
    if (newDoc["mqtt"].containsKey("port")) {
      mergedDoc["mqtt"]["port"] = newDoc["mqtt"]["port"];
    }
    if (newDoc["mqtt"].containsKey("user")) {
      mergedDoc["mqtt"]["user"] = newDoc["mqtt"]["user"];
    }
    if (newDoc["mqtt"].containsKey("password")) {
      mergedDoc["mqtt"]["password"] = newDoc["mqtt"]["password"];
    }
    if (newDoc["mqtt"].containsKey("topic_status")) {
      mergedDoc["mqtt"]["topic_status"] = newDoc["mqtt"]["topic_status"];
    }
    if (newDoc["mqtt"].containsKey("topic_control")) {
      mergedDoc["mqtt"]["topic_control"] = newDoc["mqtt"]["topic_control"];
    }
    if (newDoc["mqtt"].containsKey("security")) {
      mergedDoc["mqtt"]["security"] = newDoc["mqtt"]["security"];
    }
  }
  
  if (newDoc.containsKey("temperature")) {
    if (newDoc["temperature"].containsKey("high_threshold")) {
      mergedDoc["temperature"]["high_threshold"] = newDoc["temperature"]["high_threshold"];
    }
    if (newDoc["temperature"].containsKey("low_threshold")) {
      mergedDoc["temperature"]["low_threshold"] = newDoc["temperature"]["low_threshold"];
    }
  }
  
  if (newDoc.containsKey("timezone")) {
    if (newDoc["timezone"].containsKey("offset")) {
      mergedDoc["timezone"]["offset"] = newDoc["timezone"]["offset"];
    }
  }
  
  if (newDoc.containsKey("operation_mode")) {
    mergedDoc["operation_mode"] = newDoc["operation_mode"];
  }
  
  if (newDoc.containsKey("alert")) {
    if (newDoc["alert"].containsKey("min_temp")) {
      mergedDoc["alert"]["min_temp"] = newDoc["alert"]["min_temp"];
    }
    if (newDoc["alert"].containsKey("max_temp")) {
      mergedDoc["alert"]["max_temp"] = newDoc["alert"]["max_temp"];
    }
    if (newDoc["alert"].containsKey("buzzer_enabled")) {
      mergedDoc["alert"]["buzzer_enabled"] = newDoc["alert"]["buzzer_enabled"];
    }
  }
  
  if (newDoc.containsKey("stabilization")) {
    if (newDoc["stabilization"].containsKey("target_temp")) {
      mergedDoc["stabilization"]["target_temp"] = newDoc["stabilization"]["target_temp"];
    }
    if (newDoc["stabilization"].containsKey("tolerance")) {
      mergedDoc["stabilization"]["tolerance"] = newDoc["stabilization"]["tolerance"];
    }
    if (newDoc["stabilization"].containsKey("alert_threshold")) {
      mergedDoc["stabilization"]["alert_threshold"] = newDoc["stabilization"]["alert_threshold"];
    }
    if (newDoc["stabilization"].containsKey("duration")) {
      mergedDoc["stabilization"]["duration"] = newDoc["stabilization"]["duration"];
    }
  }
  
  // Сохраняем датчики, если они есть в новом запросе
  if (newDoc.containsKey("sensors")) {
    mergedDoc["sensors"] = newDoc["sensors"];
  }
  
  yield(); // Даем время после объединения
  
  // Применяем настройки
  if (mergedDoc["timezone"].containsKey("offset")) {
    int offset = mergedDoc["timezone"]["offset"];
    setTimezone(offset);
    yield();
  }
  if (mergedDoc.containsKey("operation_mode")) {
    int mode = mergedDoc["operation_mode"];
    setOperationMode((OperationMode)mode);
    yield();
  }
  if (mergedDoc.containsKey("telegram")) {
    String token = mergedDoc["telegram"]["bot_token"] | "";
    String chatId = mergedDoc["telegram"]["chat_id"] | "";
    if (token.length() > 0 || chatId.length() > 0) {
      setTelegramConfig(token, chatId);
      yield();
    }
  }
  if (mergedDoc.containsKey("mqtt")) {
    String server = mergedDoc["mqtt"]["server"] | "";
    server.trim();
    int port = mergedDoc["mqtt"]["port"] | 1883;
    String user = mergedDoc["mqtt"]["user"] | "";
    String password = mergedDoc["mqtt"]["password"] | "";
    String topicStatus = mergedDoc["mqtt"]["topic_status"] | "home/thermo/status";
    String topicControl = mergedDoc["mqtt"]["topic_control"] | "home/thermo/control";
    String security = mergedDoc["mqtt"]["security"] | "none";
    
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
    yield();
  }
  if (mergedDoc.containsKey("alert")) {
    float minTemp = mergedDoc["alert"]["min_temp"] | 10.0;
    float maxTemp = mergedDoc["alert"]["max_temp"] | 30.0;
    bool buzzerEnabled = mergedDoc["alert"]["buzzer_enabled"] | true;
    setAlertSettings(minTemp, maxTemp, buzzerEnabled);
    yield();
  }
  if (mergedDoc.containsKey("stabilization")) {
    float targetTemp = mergedDoc["stabilization"]["target_temp"] | 25.0;
    float tolerance = mergedDoc["stabilization"]["tolerance"] | 0.1;
    float alertThreshold = mergedDoc["stabilization"]["alert_threshold"] | 0.2;
    unsigned long duration = mergedDoc["stabilization"]["duration"] | 600;
    setStabilizationSettings(targetTemp, tolerance, alertThreshold, duration);
    yield();
  }
  
  yield(); // Даем время перед записью в файл
  
  // Сохраняем объединенные настройки в SPIFFS
  File file = SPIFFS.open(SETTINGS_FILE, "w");
  if (!file) {
    Serial.println(F("Failed to open settings file for writing"));
    return false;
  }
  
  String output;
  serializeJson(mergedDoc, output);
  
  size_t bytesWritten = file.print(output);
  file.flush(); // Принудительно записываем данные на диск
  file.close();
  
  yield(); // Даем время после записи в файл
  
  // Логируем для отладки
  Serial.print(F("Settings saved. Size: "));
  Serial.print(output.length());
  Serial.print(F(" bytes, Written: "));
  Serial.println(bytesWritten);
  
  // Откладываем запись в NVS, чтобы не блокировать WiFi
  // Запись будет выполнена в main loop через processPendingNvsSave()
  pendingNvsData = output;
  pendingNvsSave = true;
  Serial.println(F("NVS save scheduled for background processing"));

  // Проверяем, что файл действительно записался
  delay(100); // Небольшая задержка для завершения записи
  yield();
  
  File verifyFile = SPIFFS.open(SETTINGS_FILE, "r");
  if (verifyFile) {
    String verifyContent = verifyFile.readString();
    verifyFile.close();
    if (verifyContent.length() > 0 && verifyContent == output) {
      Serial.println(F("Settings file verified successfully"));
    } else {
      Serial.println(F("WARNING: Settings file verification failed!"));
      Serial.print(F("Expected length: "));
      Serial.print(output.length());
      Serial.print(F(", Got length: "));
      Serial.println(verifyContent.length());
    }
  } else {
    Serial.println(F("WARNING: Could not verify settings file!"));
  }
  
  return true;
}

// Функция для обработки отложенной записи в NVS
// Вызывается из main loop для записи настроек без блокировки WiFi
void processPendingNvsSave() {
  if (!pendingNvsSave || pendingNvsData.length() == 0) {
    return;
  }

  Serial.println(F("Processing pending NVS save..."));

  // Парсим JSON с настройками
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, pendingNvsData);

  if (error) {
    Serial.print(F("NVS save: JSON parse error: "));
    Serial.println(error.c_str());
    pendingNvsSave = false;
    pendingNvsData = "";
    return;
  }

  // Открываем NVS для записи
  if (!preferences.begin(PREF_NAMESPACE, false)) {
    Serial.println(F("NVS: Failed to open preferences"));
    pendingNvsSave = false;
    pendingNvsData = "";
    return;
  }

  // Записываем WiFi настройки с yield между операциями
  if (doc.containsKey("wifi")) {
    String ssid = doc["wifi"]["ssid"] | "";
    String pass = doc["wifi"]["password"] | "";
    if (ssid.length() > 0) {
      preferences.putString(PREF_WIFI_SSID, ssid);
      yield();
      preferences.putString(PREF_WIFI_PASS, pass);
      yield();
    }
  }

  // Записываем Telegram настройки
  if (doc.containsKey("telegram")) {
    String token = doc["telegram"]["token"] | "";
    String chatId = doc["telegram"]["chatId"] | "";
    preferences.putString(PREF_TG_TOKEN, token);
    yield();
    preferences.putString(PREF_TG_CHATID, chatId);
    yield();
  }

  // Записываем MQTT настройки
  if (doc.containsKey("mqtt")) {
    String server = doc["mqtt"]["server"] | "";
    int port = doc["mqtt"]["port"] | 1883;
    String user = doc["mqtt"]["user"] | "";
    String mqttPass = doc["mqtt"]["password"] | "";

    preferences.putString(PREF_MQTT_SERVER, server);
    yield();
    preferences.putInt(PREF_MQTT_PORT, port);
    yield();
    preferences.putString(PREF_MQTT_USER, user);
    yield();
    preferences.putString(PREF_MQTT_PASS, mqttPass);
    yield();
  }

  preferences.end();

  // Очищаем данные
  pendingNvsSave = false;
  pendingNvsData = "";

  Serial.println(F("NVS save completed"));
}

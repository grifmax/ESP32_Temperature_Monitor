#include "web_server.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
// #include <WiFiManager.h>  // Временно отключено
#include "time_manager.h"
#include "temperature_history.h"
#include "operation_modes.h"
#include "buzzer.h"
#include "tg_bot.h"
#include "mqtt_client.h"

extern float currentTemp;
extern unsigned long deviceUptime;
extern String deviceIP;
extern int wifiRSSI;
extern int displayScreen;
extern unsigned long wifiConnectedSeconds;
// extern WiFiManager wm;  // Временно отключено

AsyncWebServer server(80);

// Файл для хранения настроек
#define SETTINGS_FILE "/settings.json"

void startWebServer() {
  // Главная страница
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  // Статические файлы (без gzip)
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/style.css", "text/css");
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/script.js", "application/javascript");
  });
  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/settings.html", "text/html");
  });
  server.on("/settings.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/settings.js", "application/javascript");
  });
  
  // JSON API endpoint для получения данных
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request){
    // Оптимизировано с 512 до 384 для экономии памяти
    StaticJsonDocument<640> doc;
    
    doc["temperature"] = currentTemp;
    doc["ip"] = deviceIP;
    doc["uptime"] = deviceUptime;
    doc["wifi_status"] = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
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
    if (period == "1h") {
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
      JsonObject record = data.createNestedObject();
      record["timestamp"] = records[i].timestamp;
      record["temperature"] = records[i].temperature;
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
  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String body = String((char*)data);
      body = body.substring(0, len);
      
      if (saveSettings(body)) {
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(500, "application/json", "{\"status\":\"error\"}");
      }
    });
  
  // API для получения списка термометров
  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<512> doc;
    JsonArray sensorsArray = doc.createNestedArray("sensors");
    
    // Один термометр по умолчанию
    JsonObject sensor1 = sensorsArray.createNestedObject();
    sensor1["id"] = 1;
    sensor1["name"] = "Термометр 1";
    sensor1["enabled"] = true;
    sensor1["correction"] = 0.0;
    sensor1["mode"] = "monitoring";
    sensor1["sendToNetworks"] = true;
    sensor1["buzzerEnabled"] = false;
    sensor1["currentTemp"] = currentTemp;
    sensor1["stabilizationState"] = "tracking";
    
    // Настройки оповещения и стабилизации (минимальный набор)
    sensor1["alertSettings"]["minTemp"] = 10.0;
    sensor1["alertSettings"]["maxTemp"] = 30.0;
    sensor1["alertSettings"]["buzzerEnabled"] = true;
    sensor1["stabilizationSettings"]["targetTemp"] = 25.0;
    sensor1["stabilizationSettings"]["tolerance"] = 0.1;
    sensor1["stabilizationSettings"]["alertThreshold"] = 0.2;
    sensor1["stabilizationSettings"]["duration"] = 10;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  // API для сохранения списка термометров
  server.on("/api/sensors", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String body = String((char*)data);
      body = body.substring(0, len);
      
      StaticJsonDocument<768> doc;
      DeserializationError error = deserializeJson(doc, body);
      
      if (!error && doc.containsKey("sensors")) {
        // TODO: Сохранять в файл настроек
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
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
  server.on("^/api/sensor/([0-9]+)$", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String body = String((char*)data);
      body = body.substring(0, len);
      
      String sensorId = request->pathArg(0);
      int id = sensorId.toInt();
      
      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, body);
      
      if (!error) {
        // TODO: Сохранять настройки термометра в файл настроек
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
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
  server.on("/api/mode", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String body = String((char*)data);
      body = body.substring(0, len);
      
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, body);
      
      if (!error && doc.containsKey("mode")) {
        int mode = doc["mode"];
        setOperationMode((OperationMode)mode);
        
        if (mode == MODE_ALERT && doc.containsKey("alert")) {
          float minTemp = doc["alert"]["min_temp"] | 10.0;
          float maxTemp = doc["alert"]["max_temp"] | 30.0;
          bool buzzerEnabled = doc["alert"]["buzzer_enabled"] | true;
          setAlertSettings(minTemp, maxTemp, buzzerEnabled);
        } else if (mode == MODE_STABILIZATION && doc.containsKey("stabilization")) {
          float targetTemp = doc["stabilization"]["target_temp"] | 25.0;
          float tolerance = doc["stabilization"]["tolerance"] | 0.1;
          float alertThreshold = doc["stabilization"]["alert_threshold"] | 0.2;
          unsigned long duration = doc["stabilization"]["duration"] | 600;
          setStabilizationSettings(targetTemp, tolerance, alertThreshold, duration);
        }
        
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      }
    });
  
  // API для подключения к Wi-Fi
  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String body = String((char*)data);
      body = body.substring(0, len);
      
      StaticJsonDocument<256> doc;
      deserializeJson(doc, body);
      
      String ssid = doc["ssid"] | "";
      String password = doc["password"] | "";
      
      if (ssid.length() > 0) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(ssid.c_str(), password.c_str());
        request->send(200, "application/json", "{\"status\":\"connecting\"}");
      } else {
        request->send(400, "application/json", "{\"status\":\"invalid\"}");
      }
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

// Функция получения настроек из файла
String getSettings() {
  File file = LittleFS.open(SETTINGS_FILE, "r");
  if (!file) {
    // Возвращаем настройки по умолчанию
    StaticJsonDocument<768> doc;
    doc["wifi"]["ssid"] = "";
    doc["wifi"]["password"] = "";
    doc["mqtt"]["server"] = "";
    doc["mqtt"]["port"] = 1883;
    doc["mqtt"]["user"] = "";
    doc["mqtt"]["password"] = "";
    doc["mqtt"]["topic_status"] = "home/thermo/status";
    doc["mqtt"]["topic_control"] = "home/thermo/control";
    doc["mqtt"]["security"] = "none";
    doc["telegram"]["bot_token"] = "";
    doc["telegram"]["chat_id"] = "";
    doc["temperature"]["high_threshold"] = 30.0;
    doc["temperature"]["low_threshold"] = 10.0;
    doc["timezone"]["offset"] = 3; // UTC+3 по умолчанию
    doc["operation_mode"] = 0; // MODE_LOCAL по умолчанию
    doc["alert"]["min_temp"] = 10.0;
    doc["alert"]["max_temp"] = 30.0;
    doc["alert"]["buzzer_enabled"] = true;
    doc["stabilization"]["target_temp"] = 25.0;
    doc["stabilization"]["tolerance"] = 0.1;
    doc["stabilization"]["alert_threshold"] = 0.2;
    doc["stabilization"]["duration"] = 600;
    
    String result;
    serializeJson(doc, result);
    return result;
  }
  
  String content = file.readString();
  file.close();
  
  // Парсим и добавляем часовой пояс, если его нет
  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, content);
  if (error) {
    Serial.println(F("Failed to parse settings file"));
    return "{}";
  }
  
  if (!doc["timezone"].containsKey("offset")) {
    doc["timezone"]["offset"] = 3;
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
  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.println(F("Failed to parse settings JSON"));
    return false;
  }
  
  // Применяем настройки
  if (doc["timezone"].containsKey("offset")) {
    int offset = doc["timezone"]["offset"];
    setTimezone(offset);
  }
  if (doc.containsKey("operation_mode")) {
    int mode = doc["operation_mode"];
    setOperationMode((OperationMode)mode);
  }
  if (doc.containsKey("telegram")) {
    String token = doc["telegram"]["bot_token"] | "";
    String chatId = doc["telegram"]["chat_id"] | "";
    if (token.length() > 0 || chatId.length() > 0) {
      setTelegramConfig(token, chatId);
    }
  }
  if (doc.containsKey("mqtt")) {
    String server = doc["mqtt"]["server"] | "";
    int port = doc["mqtt"]["port"] | 1883;
    String user = doc["mqtt"]["user"] | "";
    String password = doc["mqtt"]["password"] | "";
    String topicStatus = doc["mqtt"]["topic_status"] | "home/thermo/status";
    String topicControl = doc["mqtt"]["topic_control"] | "home/thermo/control";
    String security = doc["mqtt"]["security"] | "none";
    setMqttConfig(server, port, user, password, topicStatus, topicControl, security);
  }
  if (doc.containsKey("alert")) {
    float minTemp = doc["alert"]["min_temp"] | 10.0;
    float maxTemp = doc["alert"]["max_temp"] | 30.0;
    bool buzzerEnabled = doc["alert"]["buzzer_enabled"] | true;
    setAlertSettings(minTemp, maxTemp, buzzerEnabled);
  }
  if (doc.containsKey("stabilization")) {
    float targetTemp = doc["stabilization"]["target_temp"] | 25.0;
    float tolerance = doc["stabilization"]["tolerance"] | 0.1;
    float alertThreshold = doc["stabilization"]["alert_threshold"] | 0.2;
    unsigned long duration = doc["stabilization"]["duration"] | 600;
    setStabilizationSettings(targetTemp, tolerance, alertThreshold, duration);
  }
  
  File file = LittleFS.open(SETTINGS_FILE, "w");
  if (!file) {
    Serial.println(F("Failed to open settings file for writing"));
    return false;
  }
  
  String output;
  serializeJson(doc, output);
  file.print(output);
  file.close();
  Serial.println(F("Settings saved"));
  return true;
}

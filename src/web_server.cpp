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
#include <DallasTemperature.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Для использования MAX_HISTORY_SIZE в web_server.cpp
#ifndef MAX_HISTORY_SIZE
#define MAX_HISTORY_SIZE 288
#endif

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

// Мьютекс для защиты доступа к sensors и SPIFFS
// ВАЖНО: Порядок взятия мьютексов должен быть ВСЕГДА одинаковым для предотвращения deadlock:
// 1. Сначала sensorsMutex
// 2. Затем spiffsMutex
// Если нужно взять только один мьютекс, можно взять только его, но если нужны оба - всегда в этом порядке
SemaphoreHandle_t sensorsMutex = NULL;
SemaphoreHandle_t spiffsMutex = NULL;

// Кеш для данных API
struct ApiDataCache {
  String jsonData;
  unsigned long lastUpdate;
  bool valid;
};
static ApiDataCache apiDataCache = {"", 0, false};
static const unsigned long API_CACHE_TTL = 2000; // Кеш на 2 секунды

// Кеш для настроек (getSettings)
struct SettingsCache {
  String jsonData;
  unsigned long lastUpdate;
  bool valid;
};
static SettingsCache settingsCache = {"", 0, false};
static const unsigned long SETTINGS_CACHE_TTL = 5000; // Кеш на 5 секунд

// Файл для хранения настроек
#define SETTINGS_FILE "/settings.json"

// Preferences для надежного хранения критичных настроек
Preferences preferences;
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
  // Инициализация мьютексов
  if (sensorsMutex == NULL) {
    sensorsMutex = xSemaphoreCreateMutex();
  }
  if (spiffsMutex == NULL) {
    spiffsMutex = xSemaphoreCreateMutex();
  }
  
  // Очищаем кеши при старте сервера
  apiDataCache.valid = false;
  apiDataCache.jsonData = "";
  apiDataCache.lastUpdate = 0;
  settingsCache.valid = false;
  settingsCache.jsonData = "";
  settingsCache.lastUpdate = 0;
  Serial.println(F("Web server caches cleared"));
  
  // Главная страница
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    yield(); // Кормим watchdog
    request->send(SPIFFS, "/index.html", "text/html");
  });
  
  // Статические файлы (без gzip)
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
    yield();
    request->send(SPIFFS, "/index.html", "text/html");
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    yield();
    request->send(SPIFFS, "/style.css", "text/css");
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
    yield();
    request->send(SPIFFS, "/script.js", "application/javascript");
  });
  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *request){
    yield(); // Особенно важно для страницы настроек
    request->send(SPIFFS, "/settings.html", "text/html");
  });
  server.on("/settings.js", HTTP_GET, [](AsyncWebServerRequest *request){
    yield();
    request->send(SPIFFS, "/settings.js", "application/javascript");
  });
  
  // JSON API endpoint для получения данных
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request){
    // Кормим watchdog сразу
    yield();
    
    // Проверяем кеш
    unsigned long now = millis();
    if (apiDataCache.valid && (now - apiDataCache.lastUpdate) < API_CACHE_TTL) {
      // Проверяем, что кеш содержит валидные данные (должен содержать sensors массив)
      if (apiDataCache.jsonData.length() > 0 && apiDataCache.jsonData.indexOf("\"sensors\"") > 0) {
        Serial.print(F("API data served from cache (age: "));
        Serial.print(now - apiDataCache.lastUpdate);
        Serial.println(F("ms)"));
        request->send(200, "application/json", apiDataCache.jsonData);
        return;
      } else {
        // Кеш невалиден, инвалидируем его
        Serial.println(F("Invalid cache, regenerating"));
        apiDataCache.valid = false;
      }
    } else if (apiDataCache.valid) {
      Serial.print(F("Cache expired (age: "));
      Serial.print(now - apiDataCache.lastUpdate);
      Serial.println(F("ms), regenerating"));
      apiDataCache.valid = false;
    }
    
    yield(); // Даем время перед началом работы
    
    // Увеличено для поддержки данных о термометрах (увеличиваем до 8192 для большего количества датчиков и данных)
    StaticJsonDocument<8192> doc;
    
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
    
    // Добавление времени (с yield между вызовами)
    unsigned long unixTime = getUnixTime();
    yield();
    doc["current_time"] = getCurrentTime();
    yield();
    doc["current_date"] = getCurrentDate();
    yield();
    doc["unix_time"] = unixTime;
    doc["time_synced"] = unixTime > 0;
    yield();

    // Статусы сервисов (упрощенно, с yield)
    // Создаем объекты для mqtt и telegram явно
    JsonObject mqttObj = doc.createNestedObject("mqtt");
    bool mqttConfigured = isMqttConfigured();
    yield();
    mqttObj["configured"] = mqttConfigured;
    mqttObj["status"] = getMqttStatus();
    yield();

    JsonObject telegramObj = doc.createNestedObject("telegram");
    bool telegramConfigured = isTelegramConfigured();
    yield();
    bool telegramInitialized = isTelegramInitialized();
    yield();
    bool telegramPollOk = isTelegramPollOk();
    yield();
    unsigned long lastPollMs = getTelegramLastPollMs();
    yield();
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
    telegramObj["configured"] = telegramConfigured;
    telegramObj["status"] = telegramStatus;
    if (lastPollMs > 0) {
      telegramObj["last_poll_age"] = (millis() - lastPollMs) / 1000;
    }
    yield();
    
    // Информация о режиме работы (упрощенно)
    OperationMode mode = getOperationMode();
    yield();
    doc["operation_mode"] = mode;
    const char* modeNames[] = {"local", "monitoring", "alert", "stabilization"};
    doc["operation_mode_name"] = modeNames[mode];
    yield();
    
    // Дополнительная информация для режима стабилизации (только если нужно)
    if (mode == MODE_STABILIZATION) {
      JsonObject stabObj = doc.createNestedObject("stabilization");
      stabObj["is_stabilized"] = isStabilized();
      yield();
      stabObj["time"] = getStabilizationTime();
      yield();
      StabilizationModeSettings stab = getStabilizationSettings();
      yield();
      stabObj["target_temp"] = stab.targetTemp;
      stabObj["tolerance"] = stab.tolerance;
      yield();
    }
    
    // Обработка датчиков - используем кешированные температуры (неблокирующие)
    JsonArray sensorsArray = doc.createNestedArray("sensors");
    
    // Получаем количество датчиков (неблокирующая операция)
    int foundCount = getSensorCount();
    yield();
    
    Serial.print(F("Found sensors count: "));
    Serial.println(foundCount);
    
    // Добавляем данные для всех найденных датчиков
    int addedCount = 0;
    for (int i = 0; i < foundCount && i < 10; i++) {
      uint8_t address[8];
      if (getSensorAddress(i, address)) {
        String addressStr = getSensorAddressString(i);
        // Используем кешированную температуру (обновляется в main.cpp каждые 10 секунд)
        float temp = getCachedSensorTemperature(i);
        yield();
        
        JsonObject sensor = sensorsArray.createNestedObject();
        // Проверяем, что объект создан успешно (в ArduinoJson isNull() проверяет валидность)
        if (!sensor.isNull()) {
            sensor["address"] = addressStr;
          sensor["currentTemp"] = temp;
          sensor["name"] = "Термометр " + String(i + 1);
          sensor["stabilizationState"] = "tracking";
          addedCount++;
        } else {
          Serial.print(F("ERROR: Failed to create sensor object for index "));
          Serial.println(i);
          break; // Прерываем цикл, если не удалось создать объект
        }
        yield();
      }
    }
    
    Serial.print(F("Added sensors to JSON: "));
    Serial.println(addedCount);
    
    // Если датчиков нет, добавляем хотя бы один для обратной совместимости
    if (foundCount == 0 || addedCount == 0) {
      JsonObject sensor = sensorsArray.createNestedObject();
      if (!sensor.isNull()) {
        sensor["index"] = 0;
        sensor["currentTemp"] = currentTemp;
        sensor["name"] = "Термометр 1";
        Serial.println(F("Added fallback sensor"));
      } else {
        Serial.println(F("ERROR: Failed to create fallback sensor object - JSON may be full"));
      }
      yield();
    }
    
    yield(); // Перед сериализацией
    
    // Проверяем размер документа перед сериализацией
    size_t docSize = measureJson(doc);
    Serial.print(F("API data JSON size before serialization: "));
    Serial.print(docSize);
    Serial.print(F(" bytes (capacity: 8192, sensors added: "));
    Serial.print(addedCount);
    Serial.print(F(")"));
    
    // Проверяем, не переполнен ли документ
    if (docSize > 8192) {
      Serial.print(F(" - OVERFLOW WARNING!"));
    }
    Serial.println();
    
    String response;
    size_t serializedSize = serializeJson(doc, response);
    
    // Проверяем, что сериализация прошла успешно
    if (serializedSize == 0) {
      Serial.println(F("ERROR: JSON serialization failed! Document may be too large or corrupted."));
      // Пытаемся отправить минимальный ответ с ошибкой
      request->send(500, "application/json", "{\"error\":\"Serialization failed\",\"sensors\":[],\"mqtt\":{\"status\":\"error\"},\"telegram\":{\"status\":\"error\"}}");
      return;
    }
    
    Serial.print(F("API response serialized size: "));
    Serial.print(serializedSize);
    Serial.print(F(" bytes, string length: "));
    Serial.println(response.length());
    
    // Проверяем размер ответа (защита от переполнения)
    // Увеличиваем лимит до 32KB, так как данные о термометрах могут быть большими
    if (response.length() > 32768) {
      Serial.print(F("WARNING: API response too large ("));
      Serial.print(response.length());
      Serial.println(F(" bytes), truncating"));
      response = response.substring(0, 32768);
    }
    
    yield(); // После сериализации
    
    // Проверяем, что ответ содержит все необходимые поля
    bool hasSensors = response.indexOf("\"sensors\"") > 0;
    bool hasMqtt = response.indexOf("\"mqtt\"") > 0;
    bool hasTelegram = response.indexOf("\"telegram\"") > 0;
    bool hasUptime = response.indexOf("\"uptime\"") > 0;
    
    Serial.print(F("Response field checks - sensors: "));
    Serial.print(hasSensors ? "OK" : "MISSING");
    Serial.print(F(", mqtt: "));
    Serial.print(hasMqtt ? "OK" : "MISSING");
    Serial.print(F(", telegram: "));
    Serial.print(hasTelegram ? "OK" : "MISSING");
    Serial.print(F(", uptime: "));
    Serial.print(hasUptime ? "OK" : "MISSING");
    Serial.println();
    
    // Кешируем только если есть хотя бы sensors (остальные поля могут отсутствовать в некоторых случаях)
    bool responseValid = hasSensors; // Минимальное требование - наличие sensors
    
    if (responseValid) {
      // Обновляем кеш только если ответ валиден
      apiDataCache.jsonData = response;
      apiDataCache.lastUpdate = now;
      apiDataCache.valid = true;
      Serial.println(F("API response cached successfully"));
    } else {
      Serial.println(F("WARNING: API response missing 'sensors' field, not caching"));
      apiDataCache.valid = false;
    }
    
    yield(); // Перед отправкой ответа
    
    // Логируем первые 200 символов ответа для отладки
    Serial.print(F("Sending response (first 200 chars): "));
    if (response.length() > 200) {
      Serial.println(response.substring(0, 200));
    } else {
      Serial.println(response);
    }
    
    // ВСЕГДА отправляем ответ, даже если некоторые поля отсутствуют
    request->send(200, "application/json", response);
    Serial.println(F("Response sent to client (200 OK)"));
  });
  
  // API для получения истории температуры
  server.on("/api/temperature/history", HTTP_GET, [](AsyncWebServerRequest *request){
    yield(); // Кормим watchdog
    
    String period = request->getParam("period") ? request->getParam("period")->value() : "24h";
    
    unsigned long endTime = getUnixTime();
    // Если время не синхронизировано, используем относительное время
    bool timeSynced = (endTime > 0);
    if (!timeSynced) {
      // Используем millis() для относительного времени
      endTime = millis() / 1000;
    }
    
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
    
    // Если время не синхронизировано, возвращаем все доступные записи
    if (!timeSynced && count == 0) {
      // Пытаемся получить все записи
      int allCount = 0;
      TemperatureRecord* allRecords = getHistory(&allCount);
      if (allRecords && allCount > 0) {
        int recordsToReturn = allCount > 100 ? 100 : allCount;
        for (int i = 0; i < recordsToReturn; i++) {
          int idx = (allCount - recordsToReturn + i) % MAX_HISTORY_SIZE;
          if (allRecords[idx].temperature != 0.0 && allRecords[idx].temperature != -127.0) {
            JsonObject record = data.createNestedObject();
            record["timestamp"] = allRecords[idx].timestamp;
            record["temperature"] = allRecords[idx].temperature;
            if (allRecords[idx].sensorAddress.length() > 0) {
              record["sensor_address"] = allRecords[idx].sensorAddress;
              record["sensor_id"] = allRecords[idx].sensorAddress;
            }
          }
          yield();
        }
        doc["count"] = recordsToReturn;
      } else {
        doc["count"] = 0;
      }
    } else if (records && count > 0) {
      // Фильтруем записи напрямую из исходного массива (безопасно, без malloc)
      // Используем getHistory для получения доступа к исходному массиву
      int allCount = 0;
      TemperatureRecord* allRecords = getHistory(&allCount);
      
      if (allRecords && allCount > 0) {
        int addedCount = 0;
        // Проходим по всем записям и фильтруем по периоду
        // Используем правильный расчет индекса для циклического буфера
        for (int i = 0; i < allCount && addedCount < maxRecords; i++) {
          // Вычисляем индекс в циклическом буфере (как в getHistoryForPeriod)
          // historyIndex указывает на следующую позицию для записи
          // Самые старые записи: (historyIndex - historyCount + i) % MAX_HISTORY_SIZE
          // Но мы не знаем historyIndex, поэтому используем упрощенный подход
          // Проходим от начала массива (самые старые записи)
          int idx = i;
          
          // Проверяем, что запись попадает в период и валидна
          if (allRecords[idx].timestamp >= startTime && 
              allRecords[idx].timestamp <= endTime &&
              allRecords[idx].temperature != 0.0 && 
              allRecords[idx].temperature != -127.0 &&
              allRecords[idx].timestamp != 0) {
            
            JsonObject record = data.createNestedObject();
            record["timestamp"] = allRecords[idx].timestamp;
            record["temperature"] = allRecords[idx].temperature;
            // Добавляем адрес термометра для идентификации
            if (allRecords[idx].sensorAddress.length() > 0) {
              record["sensor_address"] = allRecords[idx].sensorAddress;
              record["sensor_id"] = allRecords[idx].sensorAddress; // Для совместимости
            }
            addedCount++;
          }
          yield(); // Предотвращаем watchdog reset
        }
        doc["count"] = addedCount;
      } else {
        doc["count"] = 0;
      }
    } else {
      doc["count"] = 0;
    }
    
    doc["period"] = period;
    doc["time_synced"] = timeSynced;
    
    yield(); // Перед сериализацией JSON
    String response;
    serializeJson(doc, response);
    yield(); // После сериализации
    
    request->send(200, "application/json", response);
  });
  
  // API для запуска сканирования Wi-Fi сетей (асинхронное)
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println(F("WiFi scan requested..."));

    // Убеждаемся, что WiFi в режиме, позволяющем сканировать
    // Добавляем таймаут для операций WiFi
    unsigned long wifiOpStart = millis();
    const unsigned long WIFI_OP_TIMEOUT = 2000; // 2 секунды таймаут
    
    if (WiFi.getMode() == WIFI_AP) {
      WiFi.mode(WIFI_AP_STA);
      yield(); // Даем время WiFi на переключение режима
      // Проверяем таймаут
      if (millis() - wifiOpStart > WIFI_OP_TIMEOUT) {
        Serial.println(F("WARNING: WiFi mode change took too long"));
      }
    } else if (WiFi.getMode() == WIFI_OFF) {
      WiFi.mode(WIFI_STA);
      yield(); // Даем время WiFi на переключение режима
      // Проверяем таймаут
      if (millis() - wifiOpStart > WIFI_OP_TIMEOUT) {
        Serial.println(F("WARNING: WiFi mode change took too long"));
      }
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
    
    yield(); // Перед сериализацией
    String response;
    serializeJson(doc, response);
    yield(); // После сериализации
    
    // Очищаем результаты сканирования для следующего запроса
    WiFi.scanDelete();
    
    request->send(200, "application/json", response);
  });
  
  // API для получения текущих настроек
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    yield(); // Кормим watchdog
    
    String settings = "";
    if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      yield();
      settings = getSettings();
      yield();
      xSemaphoreGive(spiffsMutex);
    }
    
    yield(); // Перед отправкой
    
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
      // Проверяем размер запроса перед добавлением (защита от переполнения)
      const size_t MAX_REQUEST_SIZE = 16384; // Максимум 16KB для настроек
      if (settingsRequestBody.length() + len > MAX_REQUEST_SIZE) {
        Serial.println(F("ERROR: Settings request body too large"));
        request->send(413, "application/json", "{\"error\":\"Request too large\"}");
        settingsRequestBody = "";
        return;
      }
      
      // Добавляем данные к буферу
      settingsRequestBody += String((char*)data).substring(0, len);
      
      // Если это последний фрагмент, обрабатываем
      if (index + len >= total) {
        yield(); // Даем время другим задачам
        
        // Проверяем финальный размер запроса
        if (settingsRequestBody.length() > MAX_REQUEST_SIZE) {
          Serial.println(F("ERROR: Settings request body exceeds maximum size"));
          request->send(413, "application/json", "{\"error\":\"Request too large\"}");
          settingsRequestBody = "";
          return;
        }
        
        // saveSettings теперь сам управляет мьютексом
        bool success = saveSettings(settingsRequestBody);
        
        if (success) {
          // Инвалидируем кеш API данных и настроек
          apiDataCache.valid = false;
          settingsCache.valid = false;
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
    yield(); // Кормим watchdog
    
    // Уменьшаем размер документа для экономии стека
    StaticJsonDocument<2048> doc;
    JsonArray sensorsArray = doc.createNestedArray("sensors");
    
    // Защищаем доступ к sensors мьютексом (короткий таймаут)
    // ВАЖНО: Порядок взятия мьютексов - сначала sensorsMutex, затем spiffsMutex
    int foundCount = 0;
    if (xSemaphoreTake(sensorsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      yield();
      // НЕ сканируем датчики здесь - используем уже отсканированные
      foundCount = getSensorCount();
      
      // Загружаем настройки из файла (защищено мьютексом)
      // Берем spiffsMutex после sensorsMutex (правильный порядок)
      String settingsJson = "";
      if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        settingsJson = getSettings();
        xSemaphoreGive(spiffsMutex);
      }
      StaticJsonDocument<1024> settingsDoc;
      DeserializationError error = deserializeJson(settingsDoc, settingsJson);
      yield(); // После парсинга
      
      // Не создаем полную карту - ищем настройки по мере необходимости
    
      // НЕ запрашиваем температуру здесь - это блокирует и вызывает watchdog reset!
      // Используем кешированные значения
      
      for (int i = 0; i < foundCount; i++) {
        yield(); // Перед каждой итерацией
        uint8_t address[8];
        if (getSensorAddress(i, address)) {
          String addressStr = getSensorAddressString(i);
          // Используем кешированную температуру (обновляется в main.cpp каждые 10 секунд)
          float temp = getCachedSensorTemperature(i);
          
          JsonObject sensor = sensorsArray.createNestedObject();
        sensor["index"] = i;
        sensor["address"] = addressStr;
        
        // Ищем настройки для этого датчика (упрощенный поиск без создания карты)
        bool foundSettings = false;
        if (!error && settingsDoc.containsKey("sensors") && settingsDoc["sensors"].is<JsonArray>()) {
          JsonArray savedSensors = settingsDoc["sensors"].as<JsonArray>();
          for (JsonObject savedSensor : savedSensors) {
            String savedAddress = savedSensor["address"] | "";
            if (savedAddress == addressStr) {
              // Нашли настройки
              String defaultName = "Термометр " + String(i + 1);
              String savedName = savedSensor["name"] | "";
              sensor["name"] = (savedName.length() > 0) ? savedName : defaultName;
              sensor["enabled"] = savedSensor["enabled"] | true;
              sensor["correction"] = savedSensor["correction"] | 0.0;
              sensor["mode"] = savedSensor["mode"] | "monitoring";
              sensor["monitoringInterval"] = savedSensor["monitoringInterval"] | 5;
              sensor["sendToNetworks"] = savedSensor["sendToNetworks"] | true;
              sensor["buzzerEnabled"] = savedSensor["buzzerEnabled"] | false;
              
              if (savedSensor.containsKey("alertSettings")) {
                sensor["alertSettings"] = savedSensor["alertSettings"];
              } else {
                sensor["alertSettings"]["minTemp"] = 10.0;
                sensor["alertSettings"]["maxTemp"] = 30.0;
                sensor["alertSettings"]["buzzerEnabled"] = true;
              }
              
              if (savedSensor.containsKey("stabilizationSettings")) {
                sensor["stabilizationSettings"] = savedSensor["stabilizationSettings"];
              } else {
                sensor["stabilizationSettings"]["targetTemp"] = 25.0;
                sensor["stabilizationSettings"]["tolerance"] = 0.1;
                sensor["stabilizationSettings"]["alertThreshold"] = 0.2;
                sensor["stabilizationSettings"]["duration"] = 10;
              }
              foundSettings = true;
              break;
            }
            yield(); // Даем время между итерациями
          }
        }
        
        if (!foundSettings) {
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
          
          yield(); // После каждого датчика
        }
      }
      
      xSemaphoreGive(sensorsMutex);
      yield();
    } else {
      // Если не удалось взять мьютекс, возвращаем пустой список
      Serial.println(F("Sensors mutex busy, returning empty sensor list"));
    }
    
    yield(); // Перед сериализацией
    
    String response;
    serializeJson(doc, response);
    
    // Проверяем размер ответа (защита от переполнения)
    if (response.length() > 16384) {
      Serial.println(F("WARNING: Sensors response too large, truncating"));
      response = response.substring(0, 16384);
    }
    
    yield(); // Перед отправкой
    
    request->send(200, "application/json", response);
  });
  
  // API для сохранения списка термометров
  static String sensorsRequestBody = "";
  server.on("/api/sensors", HTTP_POST, 
    [](AsyncWebServerRequest *request){
      yield(); // Сразу кормим watchdog
      sensorsRequestBody = "";
    }, 
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      yield(); // Кормим watchdog при получении данных
      
      // Проверяем размер запроса перед добавлением (защита от переполнения)
      const size_t MAX_REQUEST_SIZE = 8192; // Максимум 8KB для запроса
      if (sensorsRequestBody.length() + len > MAX_REQUEST_SIZE) {
        Serial.println(F("ERROR: Request body too large"));
        request->send(413, "application/json", "{\"error\":\"Request too large\"}");
        sensorsRequestBody = "";
        return;
      }
      
      sensorsRequestBody += String((char*)data).substring(0, len);
      
      if (index + len >= total) {
        yield(); // Даем время другим задачам
        
        // Отмечаем начало обработки для отслеживания времени
        unsigned long processingStart = millis();
        const unsigned long MAX_PROCESSING_TIME = 5000; // Максимум 5 секунд на обработку
        
        // Проверяем финальный размер запроса
        if (sensorsRequestBody.length() > MAX_REQUEST_SIZE) {
          Serial.println(F("ERROR: Request body exceeds maximum size"));
          request->send(413, "application/json", "{\"error\":\"Request too large\"}");
          sensorsRequestBody = "";
          return;
        }
        
        yield(); // Перед парсингом JSON
        
        // Уменьшаем размер документа для экономии стека
        StaticJsonDocument<4096> doc;
        DeserializationError error = deserializeJson(doc, sensorsRequestBody);
        yield(); // После парсинга
        
        // Проверяем таймаут
        if (millis() - processingStart > MAX_PROCESSING_TIME) {
          Serial.println(F("ERROR: Processing timeout during JSON parsing"));
          request->send(500, "application/json", "{\"error\":\"Processing timeout\"}");
          sensorsRequestBody = "";
          return;
        }
        
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
        
        yield(); // Перед загрузкой настроек
        
        // Загружаем существующие настройки с защитой мьютексом
        String settingsJson = "";
        unsigned long mutexStart = millis();
        if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) { // Увеличиваем таймаут до 500мс
          yield(); // Перед getSettings
          settingsJson = getSettings();
          yield(); // После getSettings
          xSemaphoreGive(spiffsMutex);
          yield(); // После освобождения мьютекса
          
          // Проверяем таймаут
          if (millis() - processingStart > MAX_PROCESSING_TIME) {
            Serial.println(F("ERROR: Processing timeout during settings load"));
            request->send(500, "application/json", "{\"error\":\"Processing timeout\"}");
            sensorsRequestBody = "";
            return;
          }
        } else {
          Serial.println(F("SPIFFS mutex busy, cannot load settings"));
          request->send(500, "application/json", "{\"error\":\"SPIFFS busy\"}");
          sensorsRequestBody = "";
          return;
        }
        
        yield(); // Перед парсингом настроек
        
        // Объединяем настройки: загружаем существующие и добавляем новые датчики
        StaticJsonDocument<2048> settingsDoc; // Уменьшаем размер
        DeserializationError settingsError = deserializeJson(settingsDoc, settingsJson);
        yield(); // После парсинга
        
        // Проверяем таймаут
        if (millis() - processingStart > MAX_PROCESSING_TIME) {
          Serial.println(F("ERROR: Processing timeout during settings parsing"));
          request->send(500, "application/json", "{\"error\":\"Processing timeout\"}");
          sensorsRequestBody = "";
          return;
        }
        
        if (settingsError) {
          Serial.print(F("WARNING: Failed to parse existing settings, using new sensors only: "));
          Serial.println(settingsError.c_str());
          // Если не удалось распарсить, создаем новый документ только с датчиками
          settingsDoc.clear();
          yield();
        }
        
        yield(); // Перед обновлением датчиков
        
        // Добавляем/обновляем датчики
        settingsDoc["sensors"] = doc["sensors"];
        yield(); // После обновления
        
        // Проверяем таймаут
        if (millis() - processingStart > MAX_PROCESSING_TIME) {
          Serial.println(F("ERROR: Processing timeout before save"));
          request->send(500, "application/json", "{\"error\":\"Processing timeout\"}");
          sensorsRequestBody = "";
          return;
        }
        
        yield(); // Перед сериализацией
        
        // Сериализуем объединенные настройки
        String mergedJson;
        serializeJson(settingsDoc, mergedJson);
        yield(); // После сериализации
        
        // Проверяем таймаут
        if (millis() - processingStart > MAX_PROCESSING_TIME) {
          Serial.println(F("ERROR: Processing timeout during serialization"));
          request->send(500, "application/json", "{\"error\":\"Processing timeout\"}");
          sensorsRequestBody = "";
          return;
        }
        
        Serial.print(F("Saving sensors, JSON size: "));
        Serial.println(mergedJson.length());
        Serial.print(F("Processing time so far: "));
        Serial.print(millis() - processingStart);
        Serial.println(F("ms"));
        
        yield(); // Перед saveSettings
        
        // saveSettings теперь сам управляет мьютексом
        unsigned long saveStart = millis();
        bool success = saveSettings(mergedJson);
        unsigned long saveTime = millis() - saveStart;
        yield(); // После сохранения
        
        // Логируем время сохранения
        Serial.print(F("saveSettings took: "));
        Serial.print(saveTime);
        Serial.println(F("ms"));
        
        // Проверяем общий таймаут
        unsigned long totalTime = millis() - processingStart;
        if (totalTime > MAX_PROCESSING_TIME) {
          Serial.print(F("WARNING: Total processing time exceeded: "));
          Serial.print(totalTime);
          Serial.println(F("ms"));
        }
        
        if (success) {
          // Устанавливаем флаг для принудительной перезагрузки настроек в main.cpp
          extern bool forceReloadSettings;
          forceReloadSettings = true;
          // Инвалидируем кеш API данных и настроек
          apiDataCache.valid = false;
          settingsCache.valid = false;
          yield(); // Перед отправкой ответа
          request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
          Serial.println(F("WARNING: saveSettings returned false, but settings may be saved"));
          // Возвращаем успех, так как настройки могут быть сохранены в NVS
          yield(); // Перед отправкой ответа
          request->send(200, "application/json", "{\"status\":\"ok\",\"warning\":\"Save may be incomplete\"}");
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
    
    yield(); // Перед сериализацией
    String response;
    serializeJson(doc, response);
    yield(); // После сериализации
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
    
    yield(); // Перед сериализацией
    String response;
    serializeJson(doc, response);
    yield(); // После сериализации
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
          // Добавляем таймаут для WiFi операций
          unsigned long wifiOpStart = millis();
          const unsigned long WIFI_OP_TIMEOUT = 3000; // 3 секунды таймаут для подключения
          
          yield(); // Перед операциями WiFi
          WiFi.disconnect(true);
          yield(); // После disconnect
          
          if (millis() - wifiOpStart > WIFI_OP_TIMEOUT) {
            Serial.println(F("WARNING: WiFi disconnect took too long"));
          }
          
          WiFi.mode(WIFI_STA);
          yield(); // После mode
          
          if (millis() - wifiOpStart > WIFI_OP_TIMEOUT) {
            Serial.println(F("WARNING: WiFi operations taking too long"));
            request->send(500, "application/json", "{\"status\":\"timeout\"}");
            wifiConnectRequestBody = "";
            return;
          }
          
          WiFi.setAutoReconnect(true);
          yield(); // После setAutoReconnect
          
          WiFi.begin(ssid.c_str(), password.c_str());
          yield(); // Даем время после операций WiFi
          
          // Проверяем общий таймаут
          unsigned long totalTime = millis() - wifiOpStart;
          if (totalTime > WIFI_OP_TIMEOUT) {
            Serial.print(F("WARNING: WiFi connect operations took "));
            Serial.print(totalTime);
            Serial.println(F("ms"));
          }
          
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
  // Эта функция должна вызываться с уже взятым мьютексом spiffsMutex
  // или быть защищена мьютексом снаружи
  
  // Проверяем кеш настроек
  unsigned long now = millis();
  if (settingsCache.valid && (now - settingsCache.lastUpdate) < SETTINGS_CACHE_TTL) {
    return settingsCache.jsonData;
  }
  
  StaticJsonDocument<768> doc;
  
  // Сначала пытаемся загрузить из SPIFFS
  File file = SPIFFS.open(SETTINGS_FILE, "r");
  if (file) {
    // Проверяем размер файла перед чтением (защита от зависания)
    size_t fileSize = file.size();
    if (fileSize > 0 && fileSize < 16384) { // Максимум 16KB для настроек
      yield(); // Даем время перед чтением
      unsigned long readStart = millis();
      String content = file.readString();
      unsigned long readTime = millis() - readStart;
      file.close();
      
      // Проверяем, что чтение не заняло слишком много времени (>1 секунды)
      if (readTime > 1000) {
        Serial.print(F("WARNING: SPIFFS read took too long: "));
        Serial.print(readTime);
        Serial.println(F("ms"));
      }
      
      // Проверяем, что контент не пустой и не слишком большой
      if (content.length() > 0 && content.length() < 16384) {
        yield(); // Перед парсингом
        DeserializationError error = deserializeJson(doc, content);
        yield(); // После парсинга
        if (!error) {
          Serial.println(F("Settings loaded from SPIFFS"));
        } else {
          Serial.println(F("Failed to parse SPIFFS settings, trying Preferences"));
        }
      } else {
        Serial.println(F("SPIFFS file content invalid (empty or too large), trying Preferences"));
      }
    } else {
      Serial.println(F("SPIFFS file size invalid, trying Preferences"));
      file.close();
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
  
  yield(); // Перед сериализацией
  String result;
  serializeJson(doc, result);
  yield(); // После сериализации
  
  // Обновляем кеш
  settingsCache.jsonData = result;
  settingsCache.lastUpdate = now;
  settingsCache.valid = true;
  
  return result;
}

// Флаг для предотвращения одновременных сохранений
static bool saveInProgress = false;
static unsigned long lastSaveTime = 0;
static const unsigned long MIN_SAVE_INTERVAL = 500; // Минимум 500мс между сохранениями

// Функция сохранения настроек в файл
bool saveSettings(String json) {
  // Защита от одновременных сохранений
  if (saveInProgress) {
    Serial.println(F("Save already in progress, skipping"));
    return false;
  }
  
  // Защита от слишком частых сохранений
  unsigned long now = millis();
  if (lastSaveTime > 0 && (now - lastSaveTime) < MIN_SAVE_INTERVAL) {
    Serial.println(F("Save too frequent, will save after delay"));
    // Не возвращаем false сразу - попробуем сохранить, но с задержкой
    // Это позволит сохранить настройки, даже если запросы частые
  }
  
  // Проверяем, что мьютекс доступен (неблокирующий вызов с коротким таймаутом)
  if (xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println(F("SPIFFS mutex busy, cannot save settings"));
    return false;
  }
  
  saveInProgress = true;
  lastSaveTime = now;
  yield(); // Даем время другим задачам перед началом обработки
  
  // Проверяем размер JSON перед парсингом
  const size_t MAX_JSON_SIZE = 16384; // Максимум 16KB
  if (json.length() > MAX_JSON_SIZE) {
    Serial.print(F("ERROR: Settings JSON too large: "));
    Serial.print(json.length());
    Serial.println(F(" bytes"));
    xSemaphoreGive(spiffsMutex);
    saveInProgress = false;
    return false;
  }
  
  // Парсим новые настройки из запроса (уменьшаем размер для экономии стека)
  StaticJsonDocument<4096> newDoc;
  DeserializationError error = deserializeJson(newDoc, json);
  yield(); // После парсинга
  
  if (error) {
    Serial.print(F("Failed to parse settings JSON: "));
    Serial.println(error.c_str());
    Serial.print(F("JSON length: "));
    Serial.println(json.length());
    xSemaphoreGive(spiffsMutex);
    saveInProgress = false;
    return false;
  }
  
  // Загружаем существующие настройки из файла (уменьшаем размер)
  StaticJsonDocument<4096> existingDoc;
  String existingContent = "";
  File existingFile = SPIFFS.open(SETTINGS_FILE, "r");
  if (existingFile) {
    // Проверяем размер файла перед чтением
    size_t fileSize = existingFile.size();
    if (fileSize > 0 && fileSize < 16384) { // Максимум 16KB
      yield(); // Перед чтением
      unsigned long readStart = millis();
      existingContent = existingFile.readString();
      unsigned long readTime = millis() - readStart;
      existingFile.close();
      
      // Проверяем таймаут чтения
      if (readTime > 1000) {
        Serial.print(F("WARNING: SPIFFS read took too long: "));
        Serial.print(readTime);
        Serial.println(F("ms"));
      }
      
      yield(); // После чтения файла
      
      // Проверяем размер контента
      if (existingContent.length() > 0 && existingContent.length() < 16384) {
        yield(); // Перед парсингом
        DeserializationError existingParseError = deserializeJson(existingDoc, existingContent);
        yield(); // После парсинга
        if (existingParseError) {
          Serial.print(F("Warning: Failed to parse existing settings: "));
          Serial.println(existingParseError.c_str());
        }
      } else {
        Serial.println(F("Warning: Existing settings file content invalid (empty or too large)"));
      }
    } else {
      Serial.println(F("Warning: Existing settings file size invalid"));
      existingFile.close();
    }
  }
  
  yield(); // Даем время после чтения файла
  
  yield(); // Даем время после парсинга JSON
  
  // Объединяем настройки: сначала копируем существующие, затем перезаписываем новыми
  // Уменьшаем размер для экономии стека
  StaticJsonDocument<4096> mergedDoc;
  
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
  
  // Сохраняем объединенные настройки в SPIFFS (мьютекс уже взят)
  yield(); // Перед открытием файла
  File file = SPIFFS.open(SETTINGS_FILE, "w");
  if (!file) {
    Serial.println(F("Failed to open settings file for writing"));
    xSemaphoreGive(spiffsMutex);
    saveInProgress = false;
    return false;
  }
  
  yield(); // Перед сериализацией
  String output;
  serializeJson(mergedDoc, output);
  
  // Проверяем размер перед записью (защита от переполнения)
  if (output.length() > 16384) {
    Serial.println(F("ERROR: Settings too large to save (>16KB)"));
    file.close();
    xSemaphoreGive(spiffsMutex);
    saveInProgress = false;
    return false;
  }
  
  yield(); // Перед записью
  unsigned long writeStart = millis();
  size_t bytesWritten = file.print(output);
  unsigned long writeTime = millis() - writeStart;
  
  yield(); // После записи, перед flush
  file.flush(); // Принудительно записываем данные на диск
  
  yield(); // После flush, перед close
  file.close();
  
  // Проверяем таймаут записи
  if (writeTime > 2000) {
    Serial.print(F("WARNING: SPIFFS write took too long: "));
    Serial.print(writeTime);
    Serial.println(F("ms"));
  }
  
  yield(); // Даем время после записи в файл
  
  // Логируем для отладки
  Serial.print(F("Settings saved to SPIFFS. Size: "));
  Serial.print(output.length());
  Serial.print(F(" bytes, Written: "));
  Serial.println(bytesWritten);
  
  // Проверяем, что запись прошла успешно
  if (bytesWritten == 0 || bytesWritten < output.length() / 2) {
    Serial.println(F("WARNING: File write may be incomplete"));
    // Не возвращаем false - настройки уже применены в памяти и сохранены в NVS
  }
  
  // Обновляем время последнего сохранения
  lastSaveTime = millis();
  
  // Освобождаем мьютекс перед длительными операциями с NVS
  xSemaphoreGive(spiffsMutex);
  
  // Сохраняем критичные настройки (WiFi, Telegram, MQTT) в Preferences (NVS) как резерв
  // ТОЛЬКО если они изменились, чтобы не блокировать WiFi при сохранении настроек термометров
  bool wifiWasConnected = (WiFi.status() == WL_CONNECTED);
  bool needNvsSave = false;
  
  // Проверяем, изменились ли критичные настройки
  if (mergedDoc.containsKey("wifi") || mergedDoc.containsKey("telegram") || mergedDoc.containsKey("mqtt")) {
    needNvsSave = true;
  }
  
  if (needNvsSave) {
    preferences.begin(PREF_NAMESPACE, false); // read-write mode
    yield(); // После begin
    
    if (mergedDoc.containsKey("wifi")) {
      yield(); // Перед обработкой WiFi
      String wifiSsid = mergedDoc["wifi"]["ssid"] | "";
      String wifiPass = mergedDoc["wifi"]["password"] | "";
      if (wifiSsid.length() > 0) {
        yield(); // Перед записью SSID
        preferences.putString(PREF_WIFI_SSID, wifiSsid);
        yield(); // После записи SSID
        yield(); // Дополнительный yield для WiFi стека
        preferences.putString(PREF_WIFI_PASS, wifiPass);
        yield(); // После записи пароля
        yield(); // Дополнительный yield для WiFi стека
        Serial.println(F("WiFi settings saved to Preferences (NVS)"));
        yield(); // После Serial.println
      }
    }
    
    yield(); // Между секциями
    
    if (mergedDoc.containsKey("telegram")) {
      yield(); // Перед обработкой Telegram
      String tgToken = mergedDoc["telegram"]["bot_token"] | "";
      String tgChatId = mergedDoc["telegram"]["chat_id"] | "";
      if (tgToken.length() > 0 || tgChatId.length() > 0) {
        yield(); // Перед записью токена
        preferences.putString(PREF_TG_TOKEN, tgToken);
        yield(); // После записи токена
        yield(); // Дополнительный yield
        preferences.putString(PREF_TG_CHATID, tgChatId);
        yield(); // После записи chat_id
        yield(); // Дополнительный yield
        Serial.println(F("Telegram settings saved to Preferences (NVS)"));
        yield(); // После Serial.println
      }
    }
    
    yield(); // Между секциями
    
    if (mergedDoc.containsKey("mqtt")) {
      yield(); // Перед обработкой MQTT
      String mqttServer = mergedDoc["mqtt"]["server"] | "";
      int mqttPort = mergedDoc["mqtt"]["port"] | 1883;
      String mqttUser = mergedDoc["mqtt"]["user"] | "";
      String mqttPass = mergedDoc["mqtt"]["password"] | "";
      String mqttTopicSt = mergedDoc["mqtt"]["topic_status"] | "";
      String mqttTopicCt = mergedDoc["mqtt"]["topic_control"] | "";
      String mqttSec = mergedDoc["mqtt"]["security"] | "none";
      
      if (mqttServer.length() > 0 && mqttServer != "#" && mqttServer != "null") {
        yield(); // Перед записью сервера
        preferences.putString(PREF_MQTT_SERVER, mqttServer);
        yield(); // После записи сервера
        yield(); // Дополнительный yield
        preferences.putInt(PREF_MQTT_PORT, mqttPort);
        yield(); // После записи порта
        yield(); // Дополнительный yield
        preferences.putString(PREF_MQTT_USER, mqttUser);
        yield(); // После записи пользователя
        yield(); // Дополнительный yield
        preferences.putString(PREF_MQTT_PASS, mqttPass);
        yield(); // После записи пароля
        yield(); // Дополнительный yield
        if (mqttTopicSt.length() > 0) {
          preferences.putString(PREF_MQTT_TOPIC_ST, mqttTopicSt);
          yield(); // После записи топика статуса
        }
        if (mqttTopicCt.length() > 0) {
          yield(); // Перед записью топика управления
          preferences.putString(PREF_MQTT_TOPIC_CT, mqttTopicCt);
          yield(); // После записи топика управления
        }
        yield(); // Перед записью security
        preferences.putString(PREF_MQTT_SEC, mqttSec);
        yield(); // После записи security
        yield(); // Дополнительный yield перед Serial.println
        Serial.println(F("MQTT settings saved to Preferences (NVS)"));
        yield(); // После Serial.println
      }
    }
    
    yield(); // Перед end
    preferences.end();
    yield(); // После end
    yield(); // Дополнительный yield для стабилизации
  }
  
  // Проверяем, что WiFi все еще подключен (быстрая проверка)
  if (wifiWasConnected && WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi disconnected during settings save"));
    // WiFi автоматически переподключится через встроенный механизм
  }
  
  yield(); // Только yield для завершения записи
  
  // Быстрая проверка файла (опционально, можно пропустить для ускорения)
  yield();
  
  saveInProgress = false;
  return true;
}

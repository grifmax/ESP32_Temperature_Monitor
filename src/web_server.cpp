#include "web_server.h"
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

// Мьютекс для защиты флагов сохранения от race conditions
static SemaphoreHandle_t settingsMutex = NULL;

// Флаг для отложенной записи в NVS (чтобы не блокировать WiFi)
static bool pendingNvsSave = false;
String pendingNvsData = "";

// Флаг и данные для отложенного сохранения настроек в SPIFFS (полностью асинхронное сохранение)
static bool pendingSettingsSave = false;
String pendingSettingsData = "";
static bool settingsSaveSuccess = false;
static bool settingsSaveInProgress = false; // Защита от race condition
static unsigned long settingsSaveStartTime = 0; // Время начала сохранения для таймаута
String lastSaveError = ""; // Последняя ошибка сохранения

// Вспомогательные функции для потокобезопасного доступа к флагам
static inline bool takeSettingsMutex(TickType_t timeout = pdMS_TO_TICKS(100)) {
  if (settingsMutex == NULL) return true; // Если мьютекс не создан, пропускаем
  return xSemaphoreTake(settingsMutex, timeout) == pdTRUE;
}

static inline void giveSettingsMutex() {
  if (settingsMutex != NULL) {
    xSemaphoreGive(settingsMutex);
  }
}
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

// Forward declarations
void applySettingsFromJson(StaticJsonDocument<8192>& mergedDoc);

void startWebServer() {
  // Инициализация мьютекса для защиты флагов сохранения
  if (settingsMutex == NULL) {
    settingsMutex = xSemaphoreCreateMutex();
    if (settingsMutex == NULL) {
      Serial.println(F("WARNING: Failed to create settings mutex"));
    }
  }

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
  server.on("/chart.min.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/chart.min.js", "application/javascript");
  });
  server.on("/chartjs-plugin-zoom.min.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/chartjs-plugin-zoom.min.js", "application/javascript");
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
      StabilizationModeSettings stab = getStabilizationSettings();
      doc["stabilization"]["is_stabilized"] = isStabilized();
      doc["stabilization"]["time"] = getStabilizationTime();
      // Убрано stabilized_temp - используется per-sensor stabTargetTemp
      doc["stabilization"]["tolerance"] = stab.tolerance;
      doc["stabilization"]["alert_threshold"] = stab.alertThreshold;
      doc["stabilization"]["duration"] = stab.duration;
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
            // Обратная совместимость: поддерживаем и старый monitoringInterval, и новый monitoringThreshold
            if (savedSensor.containsKey("monitoringThreshold")) {
              sensorMap["monitoringThreshold"] = savedSensor["monitoringThreshold"] | 1.0;
            } else if (savedSensor.containsKey("monitoringInterval")) {
              // Конвертируем старый интервал в уставку
              unsigned long oldInterval = savedSensor["monitoringInterval"] | 5;
              sensorMap["monitoringThreshold"] = (oldInterval <= 5) ? 0.5 : 1.0;
            } else {
              sensorMap["monitoringThreshold"] = 1.0;
            }
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
          // Обратная совместимость: поддерживаем и старый monitoringInterval, и новый monitoringThreshold
          if (saved.containsKey("monitoringThreshold")) {
            sensor["monitoringThreshold"] = saved["monitoringThreshold"] | 1.0;
          } else if (saved.containsKey("monitoringInterval")) {
            // Конвертируем старый интервал в уставку
            unsigned long oldInterval = saved["monitoringInterval"] | 5;
            sensor["monitoringThreshold"] = (oldInterval <= 5) ? 0.5 : 1.0;
          } else {
            sensor["monitoringThreshold"] = 1.0;
          }
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
          sensor["monitoringThreshold"] = 1.0;
          sensor["sendToNetworks"] = true;
          sensor["buzzerEnabled"] = false;
          sensor["alertSettings"]["minTemp"] = 10.0;
          sensor["alertSettings"]["maxTemp"] = 30.0;
          sensor["alertSettings"]["buzzerEnabled"] = true;
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
    
    // Увеличиваем лимит записей для более детального графика
    // Для коротких периодов (до 1 часа) возвращаем все записи
    // Для длинных периодов ограничиваем до 500 записей
    int maxRecords = count;
    unsigned long periodSeconds = endTime - startTime;
    if (periodSeconds > 3600) { // Для периодов больше часа
      maxRecords = count > 500 ? 500 : count; // Максимум 500 записей для длинных периодов
    }
    // Для коротких периодов возвращаем все записи без ограничений
    
    StaticJsonDocument<8192> doc; // Увеличиваем размер документа для большего количества записей
    JsonArray data = doc.createNestedArray("data");
    
    // Собираем все валидные записи
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
      yield(); // Используем yield вместо delay для неблокирующей задержки
      delay(50); // Минимальная задержка для стабилизации WiFi
    } else if (WiFi.getMode() == WIFI_OFF) {
      WiFi.mode(WIFI_STA);
      yield();
      delay(50); // Минимальная задержка для стабилизации WiFi
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
  static size_t expectedTotal = 0;
  static bool requestInProgress = false;  // Флаг для защиты от одновременных запросов
  server.on("/api/settings", HTTP_POST,
    [](AsyncWebServerRequest *request){
      // Начало запроса - проверяем, не обрабатывается ли уже другой запрос
      if (requestInProgress) {
        AsyncWebServerResponse *response = request->beginResponse(503, "application/json",
          "{\"status\":\"error\",\"message\":\"Another request in progress\"}");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
        return;
      }
      requestInProgress = true;
      settingsRequestBody = "";
      expectedTotal = 0;
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      // Проверяем максимальный размер запроса (защита от переполнения)
      const size_t MAX_REQUEST_SIZE = 16384; // 16KB максимум
      if (total > MAX_REQUEST_SIZE) {
        Serial.print(F("ERROR: Request too large: "));
        Serial.print(total);
        Serial.println(F(" bytes"));
        settingsRequestBody = ""; // Очищаем при ошибке
        requestInProgress = false;  // Освобождаем флаг
        AsyncWebServerResponse *response = request->beginResponse(413, "application/json", "{\"status\":\"error\",\"message\":\"Request too large\"}");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
        return;
      }

      // Проверяем, не занят ли обработчик другим сохранением
      if (settingsSaveInProgress) {
        Serial.println(F("ERROR: Another save in progress"));
        settingsRequestBody = "";
        requestInProgress = false;  // Освобождаем флаг
        AsyncWebServerResponse *response = request->beginResponse(503, "application/json", "{\"status\":\"error\",\"message\":\"Another save in progress, try again later\"}");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
        return;
      }

      // Оптимизированная конкатенация - резервируем память только один раз
      if (index == 0) {
        settingsRequestBody = "";
        settingsRequestBody.reserve(total + 1);
        expectedTotal = total;
      }

      // Проверяем консистентность запроса
      if (total != expectedTotal) {
        Serial.println(F("ERROR: Request size mismatch"));
        settingsRequestBody = "";
        requestInProgress = false;  // Освобождаем флаг
        AsyncWebServerResponse *response = request->beginResponse(400, "application/json", "{\"status\":\"error\",\"message\":\"Request corrupted\"}");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
        return;
      }

      // Прямое копирование данных (без создания временных String объектов)
      for (size_t i = 0; i < len; i++) {
        settingsRequestBody += (char)data[i];
      }

      // Периодически даем время другим задачам при получении больших запросов
      if (index % 1024 == 0) {
        yield();
      }

      // Если это последний фрагмент, обрабатываем
      if (index + len >= total) {
        yield(); // Даем время другим задачам перед обработкой

        // Проверяем, что буфер не пустой
        if (settingsRequestBody.length() == 0) {
          Serial.println(F("ERROR: Empty request body"));
          requestInProgress = false;  // Освобождаем флаг
          AsyncWebServerResponse *response = request->beginResponse(400, "application/json", "{\"status\":\"error\",\"message\":\"Empty request\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
          return;
        }

        // Полная валидация JSON через парсинг (увеличен размер для сложных JSON)
        DynamicJsonDocument testDoc(4096);
        DeserializationError parseError = deserializeJson(testDoc, settingsRequestBody);

        if (parseError) {
          Serial.print(F("ERROR: Invalid settings JSON: "));
          Serial.println(parseError.c_str());
          String errorMsg = String("Invalid JSON: ") + parseError.c_str();
          settingsRequestBody = "";
          requestInProgress = false;  // Освобождаем флаг
          AsyncWebServerResponse *response = request->beginResponse(400, "application/json", "{\"status\":\"error\",\"message\":\"" + errorMsg + "\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
          return;
        }

        // Проверяем, не занят ли обработчик (повторная проверка перед постановкой в очередь)
        if (pendingSettingsSave || settingsSaveInProgress) {
          Serial.println(F("ERROR: Save queue busy"));
          settingsRequestBody = "";
          requestInProgress = false;  // Освобождаем флаг
          AsyncWebServerResponse *response = request->beginResponse(503, "application/json", "{\"status\":\"error\",\"message\":\"Save queue busy, try again later\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
          return;
        }

        // Откладываем сохранение для асинхронной обработки
        pendingSettingsData = settingsRequestBody;
        pendingSettingsSave = true;
        settingsSaveSuccess = false;
        settingsSaveStartTime = millis();
        lastSaveError = "";

        // Отвечаем клиенту с информацией о статусе
        AsyncWebServerResponse *response = request->beginResponse(202, "application/json", "{\"status\":\"accepted\",\"message\":\"Settings queued for save\"}");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);

        Serial.println(F("Settings save queued for background processing"));

        // Очищаем буфер после обработки
        settingsRequestBody = "";
        expectedTotal = 0;
        requestInProgress = false;  // Освобождаем флаг
      }
    });

  // API для проверки статуса сохранения
  server.on("/api/settings/status", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<256> doc;

    if (settingsSaveInProgress || pendingSettingsSave) {
      doc["status"] = "saving";
      doc["message"] = "Save in progress";
    } else if (settingsSaveSuccess) {
      doc["status"] = "success";
      doc["message"] = "Settings saved successfully";
    } else if (lastSaveError.length() > 0) {
      doc["status"] = "error";
      doc["message"] = lastSaveError;
    } else {
      doc["status"] = "idle";
      doc["message"] = "No pending save";
    }

    String response;
    serializeJson(doc, response);

    AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(resp);
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
          // Обратная совместимость: поддерживаем и старый monitoringInterval, и новый monitoringThreshold
          if (savedSensor.containsKey("monitoringThreshold")) {
            sensorMap["monitoringThreshold"] = savedSensor["monitoringThreshold"] | 1.0;
          } else if (savedSensor.containsKey("monitoringInterval")) {
            // Конвертируем старый интервал в уставку
            unsigned long oldInterval = savedSensor["monitoringInterval"] | 5;
            sensorMap["monitoringThreshold"] = (oldInterval <= 5) ? 0.5 : 1.0;
          } else {
            sensorMap["monitoringThreshold"] = 1.0;
          }
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
          // Обратная совместимость: поддерживаем и старый monitoringInterval, и новый monitoringThreshold
          if (saved.containsKey("monitoringThreshold")) {
            sensor["monitoringThreshold"] = saved["monitoringThreshold"] | 1.0;
          } else if (saved.containsKey("monitoringInterval")) {
            // Конвертируем старый интервал в уставку
            unsigned long oldInterval = saved["monitoringInterval"] | 5;
            sensor["monitoringThreshold"] = (oldInterval <= 5) ? 0.5 : 1.0;
          } else {
            sensor["monitoringThreshold"] = 1.0;
          }
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
          sensor["monitoringThreshold"] = 1.0;
          sensor["sendToNetworks"] = true;
          sensor["buzzerEnabled"] = false;
          sensor["alertSettings"]["minTemp"] = 10.0;
          sensor["alertSettings"]["maxTemp"] = 30.0;
          sensor["alertSettings"]["buzzerEnabled"] = true;
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
  static size_t sensorsExpectedTotal = 0;
  server.on("/api/sensors", HTTP_POST,
    [](AsyncWebServerRequest *request){
      sensorsRequestBody = "";
      sensorsExpectedTotal = 0;
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      // Оптимизированная конкатенация
      if (index == 0) {
        sensorsRequestBody = "";
        sensorsRequestBody.reserve(total + 1);
        sensorsExpectedTotal = total;
      }

      // Проверяем консистентность
      if (total != sensorsExpectedTotal) {
        sensorsRequestBody = "";
        AsyncWebServerResponse *response = request->beginResponse(400, "application/json", "{\"error\":\"Request corrupted\"}");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
        return;
      }

      // Прямое копирование данных
      for (size_t i = 0; i < len; i++) {
        sensorsRequestBody += (char)data[i];
      }

      if (index + len >= total) {
        yield(); // Даем время другим задачам

        StaticJsonDocument<8192> doc;
        DeserializationError error = deserializeJson(doc, sensorsRequestBody);

        if (error) {
          Serial.print(F("ERROR: Failed to parse sensors JSON: "));
          Serial.println(error.c_str());
          sensorsRequestBody = "";
          AsyncWebServerResponse *response = request->beginResponse(400, "application/json", "{\"error\":\"Invalid JSON\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
          return;
        }

        if (!doc.containsKey("sensors")) {
          Serial.println(F("ERROR: Missing 'sensors' key in JSON"));
          sensorsRequestBody = "";
          AsyncWebServerResponse *response = request->beginResponse(400, "application/json", "{\"error\":\"Missing 'sensors' key\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
          return;
        }

        // Загружаем существующие настройки
        String settingsJson = getSettings();
        StaticJsonDocument<8192> settingsDoc;
        DeserializationError settingsError = deserializeJson(settingsDoc, settingsJson);

        if (settingsError) {
          Serial.print(F("ERROR: Failed to parse existing settings: "));
          Serial.println(settingsError.c_str());
          sensorsRequestBody = "";
          AsyncWebServerResponse *response = request->beginResponse(500, "application/json", "{\"error\":\"Failed to load existing settings\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
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

          yield();

          AsyncWebServerResponse *response = request->beginResponse(200, "application/json", "{\"status\":\"ok\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
        } else {
          Serial.println(F("ERROR: saveSettings returned false"));
          AsyncWebServerResponse *response = request->beginResponse(500, "application/json", "{\"error\":\"Failed to save settings\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
        }

        sensorsRequestBody = "";
        sensorsExpectedTotal = 0;
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
      doc["stabilization"]["tolerance"] = stab.tolerance;
      doc["stabilization"]["alert_threshold"] = stab.alertThreshold;
      doc["stabilization"]["duration"] = stab.duration;
      doc["stabilization"]["is_stabilized"] = isStabilized();
      // Убрано stabilized_temp - используется per-sensor stabTargetTemp
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
            // Убрано target_temp из глобальных настроек - используется per-sensor
            float tolerance = doc["stabilization"]["tolerance"] | 0.1;
            float alertThreshold = doc["stabilization"]["alert_threshold"] | 0.2;
            unsigned long duration = doc["stabilization"]["duration"] | 600;
            setStabilizationSettings(tolerance, alertThreshold, duration);
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
  
  // API для прямого сохранения настроек Telegram в NVS (обходит очередь сохранения)
  server.on("/api/telegram/config", HTTP_POST,
    [](AsyncWebServerRequest *request){},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0 && len == total && len < 512) {
        String body = String((char*)data).substring(0, len);
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, body);

        if (error) {
          request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
          return;
        }

        String token = doc["bot_token"] | "";
        String chatId = doc["chat_id"] | "";

        if (token.length() == 0 || chatId.length() == 0) {
          request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"bot_token and chat_id required\"}");
          return;
        }

        // Сохраняем напрямую в NVS
        preferences.begin(PREF_NAMESPACE, false);
        preferences.putString(PREF_TG_TOKEN, token);
        preferences.putString(PREF_TG_CHATID, chatId);
        preferences.end();

        // Применяем настройки
        setTelegramConfig(token, chatId);

        Serial.println(F("Telegram config saved to NVS directly"));
        Serial.print(F("Token: "));
        Serial.println(token.length() > 0 ? "***" : "(empty)");
        Serial.print(F("Chat ID: "));
        Serial.println(chatId);

        request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Telegram config saved to NVS\"}");
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
  
  // API для прямого сохранения настроек MQTT в NVS (обходит очередь сохранения)
  server.on("/api/mqtt/config", HTTP_POST,
    [](AsyncWebServerRequest *request){},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0 && len == total && len < 1024) {
        String body = String((char*)data).substring(0, len);
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, body);

        if (error) {
          request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
          return;
        }

        String mqttServer = doc["server"] | "";
        int port = doc["port"] | 1883;
        String user = doc["user"] | "";
        String password = doc["password"] | "";
        String topicStatus = doc["topic_status"] | "home/thermo/status";
        String topicControl = doc["topic_control"] | "home/thermo/control";
        String security = doc["security"] | "none";

        // Сохраняем напрямую в NVS
        preferences.begin(PREF_NAMESPACE, false);
        preferences.putString(PREF_MQTT_SERVER, mqttServer);
        preferences.putInt(PREF_MQTT_PORT, port);
        preferences.putString(PREF_MQTT_USER, user);
        preferences.putString(PREF_MQTT_PASS, password);
        preferences.putString(PREF_MQTT_TOPIC_ST, topicStatus);
        preferences.putString(PREF_MQTT_TOPIC_CT, topicControl);
        preferences.putString(PREF_MQTT_SEC, security);
        preferences.end();

        // Применяем настройки
        setMqttConfig(mqttServer, port, user, password, topicStatus, topicControl, security);

        Serial.println(F("MQTT config saved to NVS directly"));
        Serial.print(F("Server: "));
        Serial.println(mqttServer.length() > 0 ? mqttServer : "(empty)");

        request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"MQTT config saved to NVS\"}");
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
  
  // Обработчик для OPTIONS запросов (CORS preflight) и 404
  server.onNotFound([](AsyncWebServerRequest *request){
    if (request->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse *response = request->beginResponse(200);
      response->addHeader("Access-Control-Allow-Origin", "*");
      response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
      response->addHeader("Access-Control-Allow-Headers", "Content-Type, Accept, X-Requested-With");
      response->addHeader("Access-Control-Max-Age", "86400"); // Кешируем preflight на 24 часа
      request->send(response);
    } else if (request->url() == "/favicon.ico") {
      // Отправляем пустой ответ для favicon.ico
      request->send(204);
    } else {
      // 404 для всех остальных необработанных запросов
      AsyncWebServerResponse *response = request->beginResponse(404, "text/plain", "Not Found");
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
    }
  });
  
  server.begin();
  Serial.println(F("Web server started"));
}

// Функция получения настроек из файла с резервным чтением из Preferences
String getSettings() {
  // Увеличен размер для поддержки множества датчиков с полными настройками
  StaticJsonDocument<4096> doc;
  
  // Сначала пытаемся загрузить из SPIFFS
  File file = SPIFFS.open(SETTINGS_FILE, "r");
  if (file) {
    // Оптимизированное чтение файла с ограничением размера
    size_t fileSize = file.size();
    if (fileSize > 0 && fileSize < 16384) {
      String content;
      content.reserve(fileSize + 1); // Резервируем память заранее
      while (file.available()) {
        content += (char)file.read();
        // Периодически даем время другим задачам при чтении больших файлов
        if (content.length() % 512 == 0) {
          yield();
        }
      }
      file.close();
      yield(); // Даем время после закрытия файла
      
      DeserializationError error = deserializeJson(doc, content);
      if (error) {
        // Логируем только ошибки, чтобы не засорять лог
        Serial.println(F("Failed to parse SPIFFS settings, trying Preferences"));
      }
    } else {
      file.close();
    }
    // Убрали избыточное логирование "Settings loaded from SPIFFS"
  }
  
  yield(); // Даем время перед работой с Preferences
  
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
  // Читаем MQTT темы и безопасность с обработкой ошибок (если ключи не существуют, вернутся пустые строки)
  String mqttTopicSt = "";
  String mqttTopicCt = "";
  String mqttSec = "";
  // Проверяем существование ключей перед чтением, чтобы избежать ошибок в логах
  if (preferences.isKey(PREF_MQTT_TOPIC_ST)) {
    mqttTopicSt = preferences.getString(PREF_MQTT_TOPIC_ST, "");
  }
  if (preferences.isKey(PREF_MQTT_TOPIC_CT)) {
    mqttTopicCt = preferences.getString(PREF_MQTT_TOPIC_CT, "");
  }
  if (preferences.isKey(PREF_MQTT_SEC)) {
    mqttSec = preferences.getString(PREF_MQTT_SEC, "");
  }
  
  preferences.end();
  
  // NVS имеет ПРИОРИТЕТ над SPIFFS для критичных настроек (WiFi, Telegram, MQTT)
  // Это гарантирует, что настройки не потеряются при перепрошивке файловой системы
  bool nvsUsed = false;

  // WiFi: NVS приоритет если там есть данные
  if (wifiSsid.length() > 0) {
    String spiffsSsid = doc.containsKey("wifi") ? doc["wifi"]["ssid"].as<String>() : "";
    if (spiffsSsid != wifiSsid) {
      doc["wifi"]["ssid"] = wifiSsid;
      doc["wifi"]["password"] = wifiPass;
      nvsUsed = true;
      Serial.println(F("WiFi loaded from NVS (priority)"));
    }
  }

  // Telegram: NVS приоритет если там есть данные
  if (tgToken.length() > 0) {
    String spiffsToken = doc.containsKey("telegram") ? doc["telegram"]["bot_token"].as<String>() : "";
    if (spiffsToken != tgToken || spiffsToken.length() == 0) {
      doc["telegram"]["bot_token"] = tgToken;
      doc["telegram"]["chat_id"] = tgChatId;
      nvsUsed = true;
      Serial.println(F("Telegram loaded from NVS (priority)"));
    }
  }

  // MQTT: NVS приоритет если там есть данные
  if (mqttServer.length() > 0) {
    String spiffsMqtt = doc.containsKey("mqtt") ? doc["mqtt"]["server"].as<String>() : "";
    if (spiffsMqtt != mqttServer || spiffsMqtt.length() == 0) {
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
      nvsUsed = true;
      Serial.println(F("MQTT loaded from NVS (priority)"));
    }
  }

  if (nvsUsed) {
    Serial.println(F("Critical settings restored from NVS"));
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
    // Убрано target_temp из глобальных настроек - используется per-sensor
    doc["stabilization"]["tolerance"] = 0.1;
    doc["stabilization"]["alert_threshold"] = 0.2;
    doc["stabilization"]["duration"] = 600;
  }
  
  // Применяем часовой пояс при загрузке
  if (doc["timezone"].containsKey("offset")) {
    int offset = doc["timezone"]["offset"];
    setTimezone(offset);
  }
  
  yield(); // Даем время перед сериализацией
  
  String result;
  serializeJson(doc, result);
  
  yield(); // Даем время после сериализации
  
  return result;
}

// Функция сохранения настроек в файл
bool saveSettings(String json) {
  yield(); // Даем время другим задачам перед началом обработки

  // Проверяем размер входящего JSON
  if (json.length() == 0) {
    Serial.println(F("ERROR: Empty JSON in saveSettings"));
    return false;
  }

  if (json.length() > 16384) {
    Serial.print(F("ERROR: JSON too large: "));
    Serial.println(json.length());
    return false;
  }

  // Загружаем существующие настройки из файла
  String existingContent = "";
  File existingFile = SPIFFS.open(SETTINGS_FILE, "r");
  if (existingFile) {
    // Оптимизированное чтение файла с ограничением размера и yield
    size_t fileSize = existingFile.size();
    if (fileSize > 0 && fileSize < 16384) {
      existingContent.reserve(fileSize + 1); // Резервируем память заранее
      while (existingFile.available()) {
        existingContent += (char)existingFile.read();
        // Периодически даем время другим задачам при чтении больших файлов
        if (existingContent.length() % 512 == 0) {
          yield();
        }
      }
    }
    existingFile.close();
    yield(); // Даем время после закрытия файла
  }

  yield(); // Даем время после чтения файла

  // Используем DynamicJsonDocument на heap вместо StaticJsonDocument на stack
  // чтобы избежать Stack Overflow (было 3x8KB = 24KB на stack при лимите ~5KB)
  DynamicJsonDocument* mergedDoc = new DynamicJsonDocument(8192);
  if (mergedDoc == nullptr) {
    Serial.println(F("ERROR: Failed to allocate memory for JSON"));
    return false;
  }

  // Парсим существующие настройки напрямую в mergedDoc
  if (existingContent.length() > 0 && existingContent != "null") {
    DeserializationError existingError = deserializeJson(*mergedDoc, existingContent);
    if (existingError) {
      Serial.println(F("Failed to parse existing settings, starting fresh"));
      mergedDoc->clear();
    }
    yield();
  }

  // Парсим новые настройки во временный документ
  DynamicJsonDocument* newDoc = new DynamicJsonDocument(4096);
  if (newDoc == nullptr) {
    Serial.println(F("ERROR: Failed to allocate memory for new JSON"));
    delete mergedDoc;
    return false;
  }

  yield(); // Даем время перед парсингом JSON
  DeserializationError error = deserializeJson(*newDoc, json);
  if (error) {
    Serial.print(F("Failed to parse settings JSON: "));
    Serial.println(error.c_str());
    Serial.print(F("JSON length: "));
    Serial.println(json.length());
    delete newDoc;
    delete mergedDoc;
    return false;
  }

  yield(); // Даем время после парсинга JSON
  
  // Если файл был пустой или не удалось распарсить, инициализируем значениями по умолчанию
  if (!mergedDoc->containsKey("wifi")) {
    (*mergedDoc)["wifi"]["ssid"] = "";
    (*mergedDoc)["wifi"]["password"] = "";
  }
  if (!mergedDoc->containsKey("mqtt")) {
    (*mergedDoc)["mqtt"]["server"] = "";
    (*mergedDoc)["mqtt"]["port"] = 1883;
    (*mergedDoc)["mqtt"]["user"] = "";
    (*mergedDoc)["mqtt"]["password"] = "";
    (*mergedDoc)["mqtt"]["topic_status"] = "home/thermo/status";
    (*mergedDoc)["mqtt"]["topic_control"] = "home/thermo/control";
    (*mergedDoc)["mqtt"]["security"] = "none";
  }
  if (!mergedDoc->containsKey("telegram")) {
    (*mergedDoc)["telegram"]["bot_token"] = "";
    (*mergedDoc)["telegram"]["chat_id"] = "";
  }
  if (!mergedDoc->containsKey("temperature")) {
    (*mergedDoc)["temperature"]["high_threshold"] = 30.0;
    (*mergedDoc)["temperature"]["low_threshold"] = 10.0;
  }
  if (!mergedDoc->containsKey("timezone")) {
    (*mergedDoc)["timezone"]["offset"] = 3;
  }
  if (!mergedDoc->containsKey("operation_mode")) {
    (*mergedDoc)["operation_mode"] = 0;
  }
  if (!mergedDoc->containsKey("alert")) {
    (*mergedDoc)["alert"]["min_temp"] = 10.0;
    (*mergedDoc)["alert"]["max_temp"] = 30.0;
    (*mergedDoc)["alert"]["buzzer_enabled"] = true;
  }
  if (!mergedDoc->containsKey("stabilization")) {
    // Убрано target_temp из глобальных настроек - используется per-sensor
    (*mergedDoc)["stabilization"]["tolerance"] = 0.1;
    (*mergedDoc)["stabilization"]["alert_threshold"] = 0.2;
    (*mergedDoc)["stabilization"]["duration"] = 600;
  }

  // Перезаписываем только те секции, которые есть в новом запросе (частичное обновление)
  if (newDoc->containsKey("wifi")) {
    // Обновляем только поля WiFi, которые есть в новом запросе
    if ((*newDoc)["wifi"].containsKey("ssid")) {
      (*mergedDoc)["wifi"]["ssid"] = (*newDoc)["wifi"]["ssid"];
    }
    if ((*newDoc)["wifi"].containsKey("password")) {
      (*mergedDoc)["wifi"]["password"] = (*newDoc)["wifi"]["password"];
    }
  }

  if (newDoc->containsKey("telegram")) {
    // Обновляем только поля Telegram, которые есть в новом запросе
    if ((*newDoc)["telegram"].containsKey("bot_token")) {
      (*mergedDoc)["telegram"]["bot_token"] = (*newDoc)["telegram"]["bot_token"];
    }
    if ((*newDoc)["telegram"].containsKey("chat_id")) {
      (*mergedDoc)["telegram"]["chat_id"] = (*newDoc)["telegram"]["chat_id"];
    }
  }

  if (newDoc->containsKey("mqtt")) {
    // Обновляем только поля MQTT, которые есть в новом запросе
    if ((*newDoc)["mqtt"].containsKey("server")) {
      String server = (*newDoc)["mqtt"]["server"] | "";
      server.trim();
      if (server == "#" || server == "null" || server.length() == 0) {
        (*mergedDoc)["mqtt"]["server"] = "";
      } else {
        (*mergedDoc)["mqtt"]["server"] = server;
      }
    }
    if ((*newDoc)["mqtt"].containsKey("port")) {
      (*mergedDoc)["mqtt"]["port"] = (*newDoc)["mqtt"]["port"];
    }
    if ((*newDoc)["mqtt"].containsKey("user")) {
      (*mergedDoc)["mqtt"]["user"] = (*newDoc)["mqtt"]["user"];
    }
    if ((*newDoc)["mqtt"].containsKey("password")) {
      (*mergedDoc)["mqtt"]["password"] = (*newDoc)["mqtt"]["password"];
    }
    if ((*newDoc)["mqtt"].containsKey("topic_status")) {
      (*mergedDoc)["mqtt"]["topic_status"] = (*newDoc)["mqtt"]["topic_status"];
    }
    if ((*newDoc)["mqtt"].containsKey("topic_control")) {
      (*mergedDoc)["mqtt"]["topic_control"] = (*newDoc)["mqtt"]["topic_control"];
    }
    if ((*newDoc)["mqtt"].containsKey("security")) {
      (*mergedDoc)["mqtt"]["security"] = (*newDoc)["mqtt"]["security"];
    }
  }

  if (newDoc->containsKey("temperature")) {
    if ((*newDoc)["temperature"].containsKey("high_threshold")) {
      (*mergedDoc)["temperature"]["high_threshold"] = (*newDoc)["temperature"]["high_threshold"];
    }
    if ((*newDoc)["temperature"].containsKey("low_threshold")) {
      (*mergedDoc)["temperature"]["low_threshold"] = (*newDoc)["temperature"]["low_threshold"];
    }
  }

  if (newDoc->containsKey("timezone")) {
    if ((*newDoc)["timezone"].containsKey("offset")) {
      (*mergedDoc)["timezone"]["offset"] = (*newDoc)["timezone"]["offset"];
    }
  }

  if (newDoc->containsKey("operation_mode")) {
    (*mergedDoc)["operation_mode"] = (*newDoc)["operation_mode"];
  }

  if (newDoc->containsKey("alert")) {
    if ((*newDoc)["alert"].containsKey("min_temp")) {
      (*mergedDoc)["alert"]["min_temp"] = (*newDoc)["alert"]["min_temp"];
    }
    if ((*newDoc)["alert"].containsKey("max_temp")) {
      (*mergedDoc)["alert"]["max_temp"] = (*newDoc)["alert"]["max_temp"];
    }
    if ((*newDoc)["alert"].containsKey("buzzer_enabled")) {
      (*mergedDoc)["alert"]["buzzer_enabled"] = (*newDoc)["alert"]["buzzer_enabled"];
    }
  }

  if (newDoc->containsKey("stabilization")) {
    // Убрано target_temp из глобальных настроек
    if ((*newDoc)["stabilization"].containsKey("tolerance")) {
      (*mergedDoc)["stabilization"]["tolerance"] = (*newDoc)["stabilization"]["tolerance"];
    }
    if ((*newDoc)["stabilization"].containsKey("alert_threshold")) {
      (*mergedDoc)["stabilization"]["alert_threshold"] = (*newDoc)["stabilization"]["alert_threshold"];
    }
    if ((*newDoc)["stabilization"].containsKey("duration")) {
      (*mergedDoc)["stabilization"]["duration"] = (*newDoc)["stabilization"]["duration"];
    }
  }

  // Сохраняем датчики, если они есть в новом запросе
  if (newDoc->containsKey("sensors")) {
    (*mergedDoc)["sensors"] = (*newDoc)["sensors"];
  }

  // Освобождаем память от newDoc - больше не нужен
  delete newDoc;
  newDoc = nullptr;

  yield(); // Даем время после объединения
  
  // НЕ применяем настройки здесь - это блокирует WiFi
  // Применение настроек будет выполнено в фоне после сохранения файла
  // Это предотвращает отключение WiFi при сохранении настроек
  
  yield(); // Даем время перед записью в файл
  
  // Сохраняем объединенные настройки в SPIFFS
  yield(); // Даем время перед открытием файла для записи
  File file = SPIFFS.open(SETTINGS_FILE, "w");
  if (!file) {
    Serial.println(F("Failed to open settings file for writing"));
    delete mergedDoc;
    return false;
  }

  yield(); // Даем время после открытия файла

  String output;
  serializeJson(*mergedDoc, output);

  // Освобождаем память от mergedDoc - больше не нужен
  delete mergedDoc;
  mergedDoc = nullptr;

  yield(); // Даем время после сериализации JSON

  // Буферизованная запись для больших файлов
  size_t bytesWritten = 0;
  const char* outputPtr = output.c_str();
  size_t outputLen = output.length();
  size_t chunkSize = 256; // Уменьшаем размер чанка для более частых yield

  for (size_t i = 0; i < outputLen; i += chunkSize) {
    size_t writeLen = (i + chunkSize < outputLen) ? chunkSize : (outputLen - i);
    bytesWritten += file.write((const uint8_t*)(outputPtr + i), writeLen);
    // Периодически даем время другим задачам при записи больших файлов
    if (i % chunkSize == 0) {
      yield();
    }
  }

  file.flush(); // Принудительно записываем данные на диск
  yield(); // Даем время после flush
  file.close();

  yield(); // Даем время после закрытия файла

  // Логируем для отладки
  Serial.print(F("Settings file saved. Size: "));
  Serial.print(output.length());
  Serial.print(F(" bytes, Written: "));
  Serial.println(bytesWritten);

  // Откладываем запись в NVS, чтобы не блокировать WiFi
  // Запись будет выполнена в main loop через processPendingNvsSave()
  if (takeSettingsMutex()) {
    pendingNvsData = output;
    pendingNvsSave = true;
    giveSettingsMutex();
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
    // Используем правильные ключи из JSON (bot_token и chat_id)
    String token = doc["telegram"]["bot_token"] | "";
    String chatId = doc["telegram"]["chat_id"] | "";
    if (token.length() > 0 || chatId.length() > 0) {
      preferences.putString(PREF_TG_TOKEN, token);
      yield();
      preferences.putString(PREF_TG_CHATID, chatId);
      yield();
    }
  }

  // Записываем MQTT настройки
  if (doc.containsKey("mqtt")) {
    String server = doc["mqtt"]["server"] | "";
    int port = doc["mqtt"]["port"] | 1883;
    String user = doc["mqtt"]["user"] | "";
    String mqttPass = doc["mqtt"]["password"] | "";
    String topicStatus = doc["mqtt"]["topic_status"] | "";
    String topicControl = doc["mqtt"]["topic_control"] | "";
    String security = doc["mqtt"]["security"] | "";

    preferences.putString(PREF_MQTT_SERVER, server);
    yield();
    preferences.putInt(PREF_MQTT_PORT, port);
    yield();
    preferences.putString(PREF_MQTT_USER, user);
    yield();
    preferences.putString(PREF_MQTT_PASS, mqttPass);
    yield();
    if (topicStatus.length() > 0) {
      preferences.putString(PREF_MQTT_TOPIC_ST, topicStatus);
      yield();
    }
    if (topicControl.length() > 0) {
      preferences.putString(PREF_MQTT_TOPIC_CT, topicControl);
      yield();
    }
    if (security.length() > 0) {
      preferences.putString(PREF_MQTT_SEC, security);
      yield();
    }
  }

  preferences.end();

  // Очищаем данные
  pendingNvsSave = false;
  pendingNvsData = "";

  Serial.println(F("NVS save completed"));
}

// Функция для обработки отложенного сохранения настроек в SPIFFS
// Вызывается из main loop для полностью асинхронного сохранения без блокировки HTTP обработчика
void processPendingSettingsSave() {
  if (!pendingSettingsSave || pendingSettingsData.length() == 0) {
    return;
  }

  // Проверяем таймаут (30 секунд максимум на ожидание)
  if (settingsSaveStartTime > 0 && millis() - settingsSaveStartTime > 30000) {
    Serial.println(F("ERROR: Settings save timeout"));
    pendingSettingsSave = false;
    settingsSaveInProgress = false;
    pendingSettingsData = "";
    lastSaveError = "Save timeout";
    return;
  }

  // Устанавливаем флаг, что сохранение в процессе
  settingsSaveInProgress = true;
  pendingSettingsSave = false;

  String settingsToSave = pendingSettingsData;
  pendingSettingsData = ""; // Очищаем данные сразу после копирования

  Serial.println(F("Processing pending settings save in background..."));

  // Выполняем сохранение в файл БЕЗ применения настроек
  bool fileSaved = saveSettings(settingsToSave);

  yield(); // Даем время после сохранения файла

  // Теперь применяем настройки с множественными yield() между операциями
  if (fileSaved) {
    // Парсим JSON для применения настроек
    StaticJsonDocument<8192> mergedDoc;
    DeserializationError error = deserializeJson(mergedDoc, settingsToSave);

    if (error) {
      Serial.print(F("Background apply: Failed to parse JSON: "));
      Serial.println(error.c_str());
      lastSaveError = String("JSON parse error: ") + error.c_str();
    } else {
      yield(); // Даем время после парсинга
      applySettingsFromJson(mergedDoc);
      lastSaveError = "";
    }
  } else {
    lastSaveError = "Failed to save to SPIFFS";
  }

  settingsSaveSuccess = fileSaved;
  settingsSaveInProgress = false;
  settingsSaveStartTime = 0;

  if (fileSaved) {
    Serial.println(F("Background settings save completed successfully"));
  } else {
    Serial.println(F("Background settings save failed"));
  }

  // Очищаем данные после обработки
  settingsToSave = "";
}

// Вспомогательная функция для применения настроек из JSON с множественными yield()
void applySettingsFromJson(StaticJsonDocument<8192>& mergedDoc) {
  Serial.println(F("Applying settings from saved JSON..."));
  
  // Применяем настройки с множественными yield() между операциями
  // Это предотвращает блокировку WiFi
  if (mergedDoc["timezone"].containsKey("offset")) {
    int offset = mergedDoc["timezone"]["offset"];
    setTimezone(offset);
    yield();
    delay(10); // Небольшая задержка для стабилизации
    yield();
  }
  
  if (mergedDoc.containsKey("operation_mode")) {
    int mode = mergedDoc["operation_mode"];
    setOperationMode((OperationMode)mode);
    yield();
    delay(10);
    yield();
  }
  
  if (mergedDoc.containsKey("telegram")) {
    String token = mergedDoc["telegram"]["bot_token"] | "";
    String chatId = mergedDoc["telegram"]["chat_id"] | "";
    if (token.length() > 0 || chatId.length() > 0) {
      setTelegramConfig(token, chatId);
      yield();
      delay(20); // Дополнительная задержка для Telegram
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
    
    yield();
    
    // Проверяем, что сервер не является placeholder или пустым
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
    delay(20); // Дополнительная задержка для MQTT
    yield();
  }
  
  if (mergedDoc.containsKey("alert")) {
    float minTemp = mergedDoc["alert"]["min_temp"] | 10.0;
    float maxTemp = mergedDoc["alert"]["max_temp"] | 30.0;
    bool buzzerEnabled = mergedDoc["alert"]["buzzer_enabled"] | true;
    setAlertSettings(minTemp, maxTemp, buzzerEnabled);
    yield();
    delay(10);
    yield();
  }
  
  if (mergedDoc.containsKey("stabilization")) {
    // Убрано target_temp из глобальных настроек - используется per-sensor
    float tolerance = mergedDoc["stabilization"]["tolerance"] | 0.1;
    float alertThreshold = mergedDoc["stabilization"]["alert_threshold"] | 0.2;
    unsigned long duration = mergedDoc["stabilization"]["duration"] | 600;
    setStabilizationSettings(tolerance, alertThreshold, duration);
    yield();
    delay(10);
    yield();
  }
  
  Serial.println(F("Settings application completed"));
}

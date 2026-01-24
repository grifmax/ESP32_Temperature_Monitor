#include "mqtt_client.h"
#include <WiFi.h>
#include <PubSubClient.h>

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

static String mqttServer = "";
static int mqttPort = 1883;
static String mqttUser = "";
static String mqttPassword = "";
static String mqttTopicStatus = "";
static String mqttTopicControl = "";
static String mqttSecurity = "none";
static bool mqttConfigured = false;

void initMqtt() {
  mqttClient.setServer("", 1883);
  mqttConfigured = false;
}

void setMqttConfig(const String& server, int port, const String& user, const String& password, const String& topicStatus, const String& topicControl, const String& security) {
  // Проверяем валидность адреса сервера
  String trimmedServer = server;
  trimmedServer.trim();
  
  // Проверяем, что сервер не пустой и не содержит некорректные символы
  bool isValidServer = trimmedServer.length() > 0 && 
                       trimmedServer != "#" && 
                       trimmedServer != "null" &&
                       trimmedServer.indexOf(' ') == -1; // Не содержит пробелов
  
  if (isValidServer && port > 0 && port <= 65535) {
    mqttServer = trimmedServer;
    mqttPort = port;
    mqttUser = user;
    mqttPassword = password;
    mqttTopicStatus = topicStatus;
    mqttTopicControl = topicControl;
    mqttSecurity = security;
    mqttClient.setServer(mqttServer.c_str(), mqttPort);
    mqttConfigured = true;
    Serial.print(F("MQTT configured: "));
    Serial.print(mqttServer);
    Serial.print(F(":"));
    Serial.println(mqttPort);
  } else {
    mqttServer = "";
    mqttPort = 1883;
    mqttUser = "";
    mqttPassword = "";
    mqttTopicStatus = "";
    mqttTopicControl = "";
    mqttSecurity = "none";
    mqttClient.setServer("", 1883);
    mqttConfigured = false;
    if (trimmedServer.length() > 0) {
      Serial.print(F("MQTT server invalid: '"));
      Serial.print(trimmedServer);
      Serial.println(F("' - MQTT disabled"));
    }
  }
}

void updateMqtt() {
  if (!mqttConfigured || mqttServer.length() == 0) {
    return;
  }
  
  // Дополнительная проверка валидности сервера перед подключением
  if (mqttServer == "#" || mqttServer == "null" || mqttServer.indexOf(' ') != -1) {
    mqttConfigured = false;
    return;
  }
  
  if (!mqttClient.connected()) {
    if (WiFi.status() == WL_CONNECTED) {
      String clientId = "ESP32_Thermo_" + String(random(0xffff), HEX);
      bool connected = false;
      if (mqttUser.length() > 0) {
        connected = mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPassword.c_str());
      } else {
        connected = mqttClient.connect(clientId.c_str());
      }
      
      if (connected) {
        Serial.print(F("MQTT connected to "));
        Serial.print(mqttServer);
        Serial.print(F(":"));
        Serial.println(mqttPort);
      } else {
        static unsigned long lastMqttError = 0;
        if (millis() - lastMqttError > 10000) { // Логируем ошибку не чаще раза в 10 секунд
          Serial.print(F("MQTT connection failed to "));
          Serial.print(mqttServer);
          Serial.print(F(":"));
          Serial.print(mqttPort);
          Serial.print(F(" - state: "));
          Serial.println(mqttClient.state());
          lastMqttError = millis();
        }
      }
    }
  } else {
    mqttClient.loop();
  }
}

bool isMqttConfigured() {
  return mqttConfigured && mqttServer.length() > 0;
}

bool isMqttConnected() {
  return mqttClient.connected();
}

const char* getMqttStatus() {
  if (!mqttConfigured || mqttServer.length() == 0) {
    return "disabled";
  }
  if (WiFi.status() != WL_CONNECTED) {
    return "waiting_wifi";
  }
  if (mqttClient.connected()) {
    return "connected";
  }
  return "disconnected";
}

bool sendMqttTestMessage() {
  if (!mqttConfigured || !mqttClient.connected() || mqttTopicStatus.length() == 0) {
    return false;
  }
  
  String message = "{\"type\":\"test\",\"message\":\"Test message from ESP32 Temperature Monitor\",\"timestamp\":";
  message += millis() / 1000;
  message += "}";
  
  bool result = mqttClient.publish(mqttTopicStatus.c_str(), message.c_str());
  if (result) {
    Serial.println(F("MQTT test message sent"));
  } else {
    Serial.println(F("MQTT test message failed"));
  }
  return result;
}

bool sendMqttMetrics(unsigned long uptime, float temperature, const String& ip, int rssi) {
  if (!mqttConfigured || !mqttClient.connected() || mqttTopicStatus.length() == 0) {
    return false;
  }
  
  // Форматируем аптайм
  unsigned long hours = uptime / 3600;
  unsigned long minutes = (uptime % 3600) / 60;
  unsigned long seconds = uptime % 60;
  
  String message = "{";
  message += "\"type\":\"metrics\",";
  message += "\"uptime_seconds\":" + String(uptime) + ",";
  message += "\"uptime_formatted\":\"" + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s\",";
  message += "\"temperature\":" + String(temperature, 2) + ",";
  message += "\"ip\":\"" + ip + "\",";
  message += "\"rssi\":" + String(rssi) + ",";
  message += "\"timestamp\":" + String(millis() / 1000);
  message += "}";
  
  bool result = mqttClient.publish(mqttTopicStatus.c_str(), message.c_str());
  return result;
}

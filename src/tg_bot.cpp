#include "tg_bot.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "config.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "operation_modes.h"
#include "web_server.h"

extern float currentTemp;
extern unsigned long deviceUptime;
extern String deviceIP;
extern int wifiRSSI;

WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr;
String telegramBotToken = "";
String telegramChatId = "";
String telegramActiveToken = "";
bool telegramInitialized = false;
bool telegramConfigured = false;
bool telegramCanSend = false;
bool telegramLastPollOk = false;
unsigned long telegramLastPollMs = 0;

// Структура для очереди сообщений Telegram
struct TelegramMessage {
  String chatId;
  String message;
  bool isTestMessage;
};

// Очередь для Telegram сообщений
QueueHandle_t telegramQueue = NULL;
bool telegramSendInProgress = false;

static void updateTelegramFlags() {
  telegramConfigured = telegramBotToken.length() > 0;
  telegramCanSend = telegramConfigured && telegramChatId.length() > 0;
}

void ensureTelegramBot() {
  updateTelegramFlags();
  if (!telegramConfigured) {
    telegramInitialized = false;
    if (bot) {
      delete bot;
      bot = nullptr;
    }
    telegramActiveToken = "";
    return;
  }
  if (!bot || telegramActiveToken != telegramBotToken) {
    if (bot) {
      delete bot;
    }
    bot = new UniversalTelegramBot(telegramBotToken, secured_client);
    telegramActiveToken = telegramBotToken;
  }
  telegramInitialized = true;
}

// Инициализация очереди Telegram сообщений
void initTelegramQueue() {
  if (telegramQueue == NULL) {
    telegramQueue = xQueueCreate(5, sizeof(TelegramMessage*));
    if (telegramQueue == NULL) {
      Serial.println(F("Failed to create Telegram queue"));
    }
  }
}

// Вспомогательная функция для отправки сообщения через очередь
static void sendTelegramMessageToQueue(const String& chatId, const String& message, bool isTest = false) {
  if (telegramQueue == NULL) {
    initTelegramQueue();
    if (telegramQueue == NULL) {
      return;
    }
  }
  
  TelegramMessage* msg = new TelegramMessage();
  msg->chatId = chatId;
  msg->message = message;
  msg->isTestMessage = isTest;
  
  if (xQueueSend(telegramQueue, &msg, 0) != pdTRUE) {
    delete msg; // Очередь переполнена
    Serial.println(F("Telegram queue is full, message dropped"));
  }
}

// Обработка очереди Telegram сообщений (вызывается из loop())
void processTelegramQueue() {
  if (telegramQueue == NULL || telegramSendInProgress) {
    return;
  }
  
  TelegramMessage* msg = NULL;
  if (xQueueReceive(telegramQueue, &msg, 0) == pdTRUE) {
    if (msg == NULL) return;
    
    telegramSendInProgress = true;
    
    // Проверяем подключение WiFi
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("Telegram queue: WiFi not connected, skipping message"));
      delete msg;
      telegramSendInProgress = false;
      return;
    }
    
    ensureTelegramBot();
    if (!telegramCanSend) {
      Serial.println(F("Telegram queue: Telegram not configured"));
      delete msg;
      telegramSendInProgress = false;
      return;
    }
    
    if (!bot) {
      Serial.println(F("Telegram queue: Bot not initialized"));
      delete msg;
      telegramSendInProgress = false;
      return;
    }
    
    Serial.print(F("Telegram queue: Sending message to chat "));
    Serial.print(msg->chatId);
    Serial.print(F(", length: "));
    Serial.println(msg->message.length());
    
    bool success = bot->sendMessage(msg->chatId, msg->message, "");
    
    if (msg->isTestMessage) {
      if (success) {
        Serial.println(F("Telegram test message sent successfully"));
      } else {
        Serial.println(F("Telegram test message failed - check bot token and chat ID"));
      }
    } else {
      Serial.println(success ? F("Telegram message sent") : F("Telegram message failed"));
    }
    
    delete msg;
    telegramSendInProgress = false;
  }
}

void startTelegramBot() {
  // Настройка SSL для Telegram
  // Для ESP32 можно использовать setInsecure() для тестирования
  // В продакшене следует использовать setCACert() с правильным сертификатом
  secured_client.setInsecure(); // Используется для тестирования
  ensureTelegramBot();
  initTelegramQueue(); // Инициализируем очередь
  if (telegramConfigured) {
    Serial.println(F("Telegram bot initialized"));
  } else {
    Serial.println(F("Telegram bot not configured"));
  }
}

void handleTelegramMessages() {
  ensureTelegramBot();
  if (!bot || !telegramConfigured) {
    return;
  }

  // Используем long poll для уменьшения количества запросов
  // 0 означает короткий poll, но мы вызываем функцию реже
  int numNewMessages = bot->getUpdates(0);
  telegramLastPollMs = millis();
  telegramLastPollOk = (numNewMessages >= 0);
  if (numNewMessages <= 0) {
    return;
  }

  for (int i = 0; i < numNewMessages; i++) {
    String text = bot->messages[i].text;
    String chat_id = String(bot->messages[i].chat_id);
    
    // Приводим команду к нижнему регистру для удобства
    text.toLowerCase();
    text.trim();
    
    if (text == "/start" || text == "/help") {
      String message = "🌡️ *ESP32 Temperature Monitor*\n\n";
      message += "📋 *Доступные команды:*\n\n";
      message += "🔹 `/status` - текущий статус устройства\n";
      message += "🔹 `/temp` - текущая температура\n";
      message += "🔹 `/sensors` - список всех датчиков\n";
      message += "🔹 `/info` - подробная информация\n";
      message += "🔹 `/mode` - текущий режим работы\n";
      message += "🔹 `/wifi` - информация о WiFi\n";
      message += "🔹 `/mqtt` - статус MQTT\n";
      message += "🔹 `/help` - эта справка\n";
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (text == "/status" || text == "/temp") {
      String message = "🌡️ *Температура:* " + String(currentTemp, 1) + "°C";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (text == "/sensors") {
      String message = "🌡️ *Датчики температуры*\n\n";
      
      // Получаем информацию о датчиках через API
      String sensorsJson = "";
      // Простой способ - используем текущую температуру
      message += "📊 *Датчик 1*\n";
      message += "   Температура: " + String(currentTemp, 1) + "°C\n";
      message += "   Статус: " + String(currentTemp > -127 ? "✅ Активен" : "❌ Ошибка");
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (text == "/info") {
      unsigned long hours = deviceUptime / 3600;
      unsigned long minutes = (deviceUptime % 3600) / 60;
      unsigned long seconds = deviceUptime % 60;
      
      String message = "📊 *Информация об устройстве*\n\n";
      message += "🌡️ *Температура:* " + String(currentTemp, 1) + "°C\n";
      message += "🌐 *IP адрес:* " + deviceIP + "\n";
      message += "⏱️ *Время работы:* " + String(hours) + "ч " + String(minutes) + "м " + String(seconds) + "с\n";
      message += "📶 *Wi-Fi RSSI:* " + String(wifiRSSI) + " dBm\n";
      message += "📡 *Wi-Fi SSID:* " + String(WiFi.SSID()) + "\n";
      
      // Режим работы
      OperationMode mode = getOperationMode();
      const char* modeNames[] = {"Локальный", "Мониторинг", "Оповещение", "Стабилизация"};
      message += "⚙️ *Режим:* " + String(modeNames[mode]) + "\n";
      
      // MQTT статус
      message += "📨 *MQTT:* " + String(isMqttConfigured() ? (isMqttConnected() ? "✅ Подключен" : "⚠️ Настроен, но не подключен") : "❌ Не настроен");
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (text == "/mode") {
      OperationMode mode = getOperationMode();
      const char* modeNames[] = {"Локальный", "Мониторинг", "Оповещение", "Стабилизация"};
      const char* modeDescs[] = {
        "Только локальный мониторинг",
        "Мониторинг с отправкой в MQTT/Telegram",
        "Режим оповещения при превышении порогов",
        "Режим стабилизации температуры"
      };
      
      String message = "⚙️ *Режим работы*\n\n";
      message += "📌 *Текущий режим:* " + String(modeNames[mode]) + "\n";
      message += "📝 *Описание:* " + String(modeDescs[mode]) + "\n\n";
      
      // Дополнительная информация в зависимости от режима
      if (mode == MODE_ALERT) {
        AlertModeSettings alert = getAlertSettings();
        message += "🔔 *Настройки оповещения:*\n";
        message += "   Мин: " + String(alert.minTemp, 1) + "°C\n";
        message += "   Макс: " + String(alert.maxTemp, 1) + "°C\n";
        message += "   Зуммер: " + String(alert.buzzerEnabled ? "✅" : "❌");
      } else if (mode == MODE_STABILIZATION) {
        StabilizationModeSettings stab = getStabilizationSettings();
        message += "🎯 *Настройки стабилизации:*\n";
        message += "   Целевая: " + String(stab.targetTemp, 1) + "°C\n";
        message += "   Допуск: " + String(stab.tolerance, 2) + "°C\n";
        message += "   Порог тревоги: " + String(stab.alertThreshold, 2) + "°C\n";
        message += "   Длительность: " + String(stab.duration) + "с";
      }
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (text == "/wifi") {
      String message = "📶 *Информация о WiFi*\n\n";
      
      if (WiFi.status() == WL_CONNECTED) {
        message += "✅ *Статус:* Подключен\n";
        message += "📡 *SSID:* " + String(WiFi.SSID()) + "\n";
        message += "🌐 *IP адрес:* " + deviceIP + "\n";
        message += "📊 *RSSI:* " + String(wifiRSSI) + " dBm\n";
        message += "🔐 *Канал:* " + String(WiFi.channel()) + "\n";
        message += "📡 *MAC:* " + String(WiFi.macAddress());
      } else {
        message += "❌ *Статус:* Не подключен\n";
        message += "⚠️ Устройство работает в режиме точки доступа";
      }
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (text == "/mqtt") {
      String message = "📨 *Статус MQTT*\n\n";
      
      if (isMqttConfigured()) {
        message += "✅ *Настроен:* Да\n";
        message += "📊 *Статус:* " + String(getMqttStatus()) + "\n";
        message += "🔌 *Подключен:* " + String(isMqttConnected() ? "✅ Да" : "❌ Нет");
      } else {
        message += "❌ *Настроен:* Нет\n";
        message += "⚠️ MQTT не настроен. Используйте веб-интерфейс для настройки.";
      }
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else {
      // Неизвестная команда
      String message = "❓ Неизвестная команда: `" + text + "`\n\n";
      message += "Используйте `/help` для списка доступных команд.";
      sendTelegramMessageToQueue(chat_id, message);
    }
  }
}

void sendMetricsToTelegram() {
  if (WiFi.status() != WL_CONNECTED) {
    return; // WiFi не подключен
  }
  
  ensureTelegramBot();
  if (!telegramCanSend) {
    return; // Telegram не настроен
  }
  
  unsigned long hours = deviceUptime / 3600;
  unsigned long minutes = (deviceUptime % 3600) / 60;
  
  String message = "📊 Метрики устройства:\n\n";
  message += "🌡️ Температура: " + String(currentTemp, 1) + "°C\n";
  message += "🌐 IP: " + deviceIP + "\n";
  message += "⏱️ Время работы: " + String(hours) + "ч " + String(minutes) + "м\n";
  message += "📶 Wi-Fi RSSI: " + String(wifiRSSI) + " dBm";
  
  // Используем очередь для отправки метрик
  sendTelegramMessageToQueue(telegramChatId, message);
}

void sendTemperatureAlert(float temperature) {
  ensureTelegramBot();
  if (!telegramCanSend) {
    return; // Telegram не настроен
  }
  
  String alert = "⚠️ *Температурное оповещение*\n\n";
  if (temperature >= HIGH_TEMP_THRESHOLD) {
    alert += "🔥 *Высокая температура!*\n";
  } else if (temperature <= LOW_TEMP_THRESHOLD) {
    alert += "❄️ *Низкая температура!*\n";
  }
  alert += "🌡️ Температура: " + String(temperature, 1) + "°C\n";
  alert += "⏰ Время: " + String(millis() / 1000) + "с";
  
  sendTelegramMessageToQueue(telegramChatId, alert);
}

bool sendTelegramTestMessage() {
  // Проверяем подключение WiFi перед отправкой
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi not connected, cannot send Telegram message"));
    return false;
  }
  
  ensureTelegramBot();
  if (!telegramCanSend) {
    Serial.println(F("Telegram not configured"));
    return false;
  }
  
  String message = "✅ *Тестовое сообщение*\n\n";
  message += "Если вы получили это сообщение, значит Telegram-бот настроен правильно!\n\n";
  message += "🌡️ Температура: " + String(currentTemp, 1) + "°C\n";
  message += "🌐 IP: " + deviceIP;
  
  sendTelegramMessageToQueue(telegramChatId, message, true);
  
  Serial.println(F("Telegram test message queued"));
  return true; // Сообщение добавлено в очередь, будет отправлено в loop()
}

void setTelegramConfig(const String& token, const String& chatId) {
  telegramBotToken = token;
  telegramChatId = chatId;
  ensureTelegramBot();
}

bool isTelegramConfigured() {
  updateTelegramFlags();
  return telegramConfigured;
}

bool isTelegramInitialized() {
  return telegramInitialized;
}

bool isTelegramPollOk() {
  return telegramLastPollOk;
}

unsigned long getTelegramLastPollMs() {
  return telegramLastPollMs;
}

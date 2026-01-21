#include "tg_bot.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "config.h"
#include <ArduinoJson.h>

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

static void updateTelegramFlags() {
  telegramConfigured = telegramBotToken.length() > 0;
  telegramCanSend = telegramConfigured && telegramChatId.length() > 0;
}

static void ensureTelegramBot() {
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

void startTelegramBot() {
  // Настройка SSL для Telegram
  // Для ESP32 можно использовать setInsecure() для тестирования
  // В продакшене следует использовать setCACert() с правильным сертификатом
  secured_client.setInsecure(); // Используется для тестирования
  ensureTelegramBot();
  if (telegramConfigured) {
    Serial.println(F("Telegram bot initialized"));
  } else {
    Serial.println(F("Telegram bot not configured"));
  }
}

void handleTelegramMessages() {
  ensureTelegramBot();
  if (!bot) {
    return;
  }

  int numNewMessages = bot->getUpdates(0);
  telegramLastPollMs = millis();
  telegramLastPollOk = (numNewMessages >= 0);
  if (numNewMessages <= 0) {
    return;
  }

  for (int i = 0; i < numNewMessages; i++) {
    String text = bot->messages[i].text;
    String chat_id = String(bot->messages[i].chat_id);
    
    if (text == "/start" || text == "/help") {
      String message = "🌡️ ESP32 Temperature Monitor\n\n";
      message += "Доступные команды:\n";
      message += "/status - текущий статус\n";
      message += "/temp - текущая температура\n";
      message += "/info - информация об устройстве";
      bot->sendMessage(String(chat_id), message, "");
    } else if (text == "/status" || text == "/temp") {
      String message = "🌡️ Температура: " + String(currentTemp, 1) + "°C";
      bot->sendMessage(String(chat_id), message, "");
    } else if (text == "/info") {
      unsigned long hours = deviceUptime / 3600;
      unsigned long minutes = (deviceUptime % 3600) / 60;
      String message = "📊 Информация об устройстве:\n\n";
      message += "🌡️ Температура: " + String(currentTemp, 1) + "°C\n";
      message += "🌐 IP: " + deviceIP + "\n";
      message += "⏱️ Время работы: " + String(hours) + "ч " + String(minutes) + "м\n";
      message += "📶 Wi-Fi RSSI: " + String(wifiRSSI) + " dBm";
      bot->sendMessage(String(chat_id), message, "");
    }
  }
}

void sendMetricsToTelegram() {
  ensureTelegramBot();
  if (!telegramCanSend || !bot) {
    return; // Telegram не настроен
  }
  
  unsigned long hours = deviceUptime / 3600;
  unsigned long minutes = (deviceUptime % 3600) / 60;
  
  String message = "📊 Метрики устройства:\n\n";
  message += "🌡️ Температура: " + String(currentTemp, 1) + "°C\n";
  message += "🌐 IP: " + deviceIP + "\n";
  message += "⏱️ Время работы: " + String(hours) + "ч " + String(minutes) + "м\n";
  message += "📶 Wi-Fi RSSI: " + String(wifiRSSI) + " dBm";
  
  bot->sendMessage(telegramChatId, message, "");
}

void sendTemperatureAlert(float temperature) {
  ensureTelegramBot();
  if (!telegramCanSend || !bot) {
    return; // Telegram не настроен
  }
  
  String alert = "⚠️ ";
  if (temperature >= HIGH_TEMP_THRESHOLD) {
    alert += "Высокая температура! ";
  } else if (temperature <= LOW_TEMP_THRESHOLD) {
    alert += "Низкая температура! ";
  }
  alert += String(temperature, 1) + "°C";
  
  bot->sendMessage(telegramChatId, alert, "");
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

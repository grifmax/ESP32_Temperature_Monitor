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
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, secured_client);

void startTelegramBot() {
  // Настройка SSL для Telegram
  // Для ESP32 можно использовать setInsecure() для тестирования
  // В продакшене следует использовать setCACert() с правильным сертификатом
  secured_client.setInsecure(); // Используется для тестирования
  Serial.println(F("Telegram bot initialized"));
}

void handleTelegramMessages() {
  int numNewMessages = bot.getUpdates(0);
  for (int i = 0; i < numNewMessages; i++) {
    String text = bot.messages[i].text;
    String chat_id = String(bot.messages[i].chat_id);
    
    if (text == "/start" || text == "/help") {
      String message = "🌡️ ESP32 Temperature Monitor\n\n";
      message += "Доступные команды:\n";
      message += "/status - текущий статус\n";
      message += "/temp - текущая температура\n";
      message += "/info - информация об устройстве";
      bot.sendMessage(String(chat_id), message, "");
    } else if (text == "/status" || text == "/temp") {
      String message = "🌡️ Температура: " + String(currentTemp, 1) + "°C";
      bot.sendMessage(String(chat_id), message, "");
    } else if (text == "/info") {
      unsigned long hours = deviceUptime / 3600;
      unsigned long minutes = (deviceUptime % 3600) / 60;
      String message = "📊 Информация об устройстве:\n\n";
      message += "🌡️ Температура: " + String(currentTemp, 1) + "°C\n";
      message += "🌐 IP: " + deviceIP + "\n";
      message += "⏱️ Время работы: " + String(hours) + "ч " + String(minutes) + "м\n";
      message += "📶 Wi-Fi RSSI: " + String(wifiRSSI) + " dBm";
      bot.sendMessage(String(chat_id), message, "");
    }
  }
}

void sendMetricsToTelegram() {
  if (strlen(TELEGRAM_BOT_TOKEN) == 0 || strlen(TELEGRAM_CHAT_ID) == 0) {
    return; // Telegram не настроен
  }
  
  unsigned long hours = deviceUptime / 3600;
  unsigned long minutes = (deviceUptime % 3600) / 60;
  
  String message = "📊 Метрики устройства:\n\n";
  message += "🌡️ Температура: " + String(currentTemp, 1) + "°C\n";
  message += "🌐 IP: " + deviceIP + "\n";
  message += "⏱️ Время работы: " + String(hours) + "ч " + String(minutes) + "м\n";
  message += "📶 Wi-Fi RSSI: " + String(wifiRSSI) + " dBm";
  
  String chatId = String(TELEGRAM_CHAT_ID);
  bot.sendMessage(chatId, message, "");
}

void sendTemperatureAlert(float temperature) {
  if (strlen(TELEGRAM_BOT_TOKEN) == 0 || strlen(TELEGRAM_CHAT_ID) == 0) {
    return; // Telegram не настроен
  }
  
  String alert = "⚠️ ";
  if (temperature >= HIGH_TEMP_THRESHOLD) {
    alert += "Высокая температура! ";
  } else if (temperature <= LOW_TEMP_THRESHOLD) {
    alert += "Низкая температура! ";
  }
  alert += String(temperature, 1) + "°C";
  
  String chatId = String(TELEGRAM_CHAT_ID);
  bot.sendMessage(chatId, alert, "");
}

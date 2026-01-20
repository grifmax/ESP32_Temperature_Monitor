#include "tg_bot.h"
#include <WiFi.h>
#include <UniversalTelegramBot.h>
#include "config.h"

WiFiClientSecure secured_client;
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, secured_client);

void startTelegramBot() {
  // Подключение к Telegram
}

void handleTelegramMessages() {
  int numNewMessages = bot.getUpdates(bot.last_update_id + 1);
  for (int i = 0; i < numNewMessages; i++) {
    String text = bot.messages[i].text;
    long chat_id = bot.messages[i].chat_id;
    if (text == "/status") {
      bot.sendMessage(chat_id, "Current Temp: " + String(currentTemp), "");
    }
  }
}

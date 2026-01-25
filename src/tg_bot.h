#ifndef TG_BOT_H
#define TG_BOT_H

#include <UniversalTelegramBot.h>

void startTelegramBot();
void handleTelegramMessages();
void sendMetricsToTelegram();
void sendMetricsToTelegram(const String& sensorName, float temperature);
void setTelegramConfig(const String& token, const String& chatId);
bool isTelegramConfigured();
bool isTelegramInitialized();
bool isTelegramPollOk();
unsigned long getTelegramLastPollMs();
void sendTemperatureAlert(float temperature);
void sendTemperatureAlert(const String& sensorName, float temperature, const String& alertType);
bool sendTelegramTestMessage();
void processTelegramQueue(); // Обработка очереди сообщений (вызывать из loop())

#endif

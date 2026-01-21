#ifndef TG_BOT_H
#define TG_BOT_H

#include <UniversalTelegramBot.h>

void startTelegramBot();
void handleTelegramMessages();
void sendMetricsToTelegram();
void setTelegramConfig(const String& token, const String& chatId);
bool isTelegramConfigured();
bool isTelegramInitialized();
bool isTelegramPollOk();
unsigned long getTelegramLastPollMs();
void sendTemperatureAlert(float temperature);

#endif

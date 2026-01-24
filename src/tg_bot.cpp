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
#include "display.h"
#include "buzzer.h"

extern float currentTemp;
extern unsigned long deviceUptime;
extern String deviceIP;
extern int wifiRSSI;
extern int displayScreen;

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
  } else {
    Serial.print(F("Message queued for chat: "));
    Serial.print(chatId);
    Serial.print(F(", length: "));
    Serial.println(message.length());
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
    
    // Используем Markdown для форматирования сообщений
    // Если Markdown не работает, можно попробовать "HTML" или убрать форматирование ""
    String parseMode = "Markdown";
    bool success = bot->sendMessage(msg->chatId, msg->message, parseMode);
    
    if (msg->isTestMessage) {
      if (success) {
        Serial.println(F("Telegram test message sent successfully"));
      } else {
        Serial.println(F("Telegram test message failed - check bot token and chat ID"));
        // Попробуем отправить без форматирования
        Serial.println(F("Trying to send without formatting..."));
        success = bot->sendMessage(msg->chatId, msg->message, "");
        Serial.println(success ? F("Message sent without formatting") : F("Still failed"));
      }
    } else {
      if (success) {
        Serial.println(F("Telegram message sent"));
      } else {
        Serial.println(F("Telegram message failed - trying without formatting..."));
        // Попробуем отправить без форматирования, если Markdown не работает
        success = bot->sendMessage(msg->chatId, msg->message, "");
        Serial.println(success ? F("Message sent without formatting") : F("Still failed"));
      }
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

  // Используем offset для получения только новых сообщений
  // last_message_received содержит ID последнего обработанного сообщения
  // Передаем last_message_received + 1, чтобы получить только новые сообщения
  int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
  telegramLastPollMs = millis();
  telegramLastPollOk = (numNewMessages >= 0);
  
  if (numNewMessages < 0) {
    Serial.println(F("Telegram getUpdates error"));
    return;
  }
  
  if (numNewMessages == 0) {
    // Нет новых сообщений - это нормально
    return;
  }
  
  Serial.print(F("Telegram: received "));
  Serial.print(numNewMessages);
  Serial.print(F(" new message(s), last_update_id: "));
  Serial.println(bot->last_message_received);

  for (int i = 0; i < numNewMessages; i++) {
    String originalText = bot->messages[i].text;
    String chat_id = String(bot->messages[i].chat_id);
    
    // Отладочный вывод
    Serial.print(F("Telegram message received: "));
    Serial.print(originalText);
    Serial.print(F(" from chat: "));
    Serial.println(chat_id);
    
    // Обработка команды с именем бота (например /start@botname)
    String text = originalText;
    int atIndex = text.indexOf('@');
    if (atIndex > 0) {
      text = text.substring(0, atIndex);
    }
    
    // Обработка команды с параметрами (берем только первую часть до пробела)
    int spaceIndex = text.indexOf(' ');
    String command = text;
    if (spaceIndex > 0) {
      command = text.substring(0, spaceIndex);
    }
    
    // Приводим команду к нижнему регистру для удобства
    command.toLowerCase();
    command.trim();
    
    // Добавляем слэш, если его нет (для удобства пользователя)
    if (command.length() > 0 && command.charAt(0) != '/') {
      command = "/" + command;
    }
    
    Serial.print(F("Processing command: "));
    Serial.println(command);
    
    if (command == "/start" || command == "/help" || command == "help" || command == "start") {
      Serial.println(F("Command /start or /help recognized, sending response..."));
      String message = "🌡️ *ESP32 Temperature Monitor*\n\n";
      message += "📋 *Информационные команды:*\n";
      message += "🔹 `/status` - текущий статус устройства\n";
      message += "🔹 `/temp` - текущая температура\n";
      message += "🔹 `/sensors` - список всех датчиков\n";
      message += "🔹 `/info` - подробная информация\n";
      message += "🔹 `/mode` - текущий режим работы\n";
      message += "🔹 `/wifi` - информация о WiFi\n";
      message += "🔹 `/mqtt` - статус MQTT\n\n";
      message += "⚙️ *Команды управления режимами:*\n";
      message += "🔹 `/mode_local` - локальный режим\n";
      message += "🔹 `/mode_monitoring` - режим мониторинга\n";
      message += "🔹 `/mode_alert` - режим оповещения\n";
      message += "🔹 `/mode_stabilization` - режим стабилизации\n\n";
      message += "🔔 *Настройка оповещений:*\n";
      message += "🔹 `/alert_set <min> <max> [buzzer]` - установить пороги\n";
      message += "   Пример: `/alert_set 10 30 1`\n";
      message += "🔹 `/alert_get` - текущие настройки\n\n";
      message += "🎯 *Настройка стабилизации:*\n";
      message += "🔹 `/stab_set <target> [tolerance] [alert] [duration]`\n";
      message += "   Пример: `/stab_set 25 0.1 0.2 600`\n";
      message += "🔹 `/stab_get` - текущие настройки\n\n";
      message += "📺 *Управление дисплеем:*\n";
      message += "🔹 `/display_on` - включить дисплей\n";
      message += "🔹 `/display_off` - выключить дисплей\n";
      message += "🔹 `/display_temp` - показать температуру\n";
      message += "🔹 `/display_info` - показать информацию\n\n";
      message += "🔊 *Управление зуммером:*\n";
      message += "🔹 `/buzzer_test` - тест зуммера\n\n";
      message += "🛠️ *Системные команды:*\n";
      message += "🔹 `/reboot` - перезагрузить устройство\n";
      message += "🔹 `/help` - эта справка\n";
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/status" || command == "/temp" || command == "status" || command == "temp") {
      String message = "🌡️ *Температура:* " + String(currentTemp, 1) + "°C";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/sensors" || command == "sensors") {
      String message = "🌡️ *Датчики температуры*\n\n";
      
      // Получаем информацию о датчиках через API
      String sensorsJson = "";
      // Простой способ - используем текущую температуру
      message += "📊 *Датчик 1*\n";
      message += "   Температура: " + String(currentTemp, 1) + "°C\n";
      message += "   Статус: " + String(currentTemp > -127 ? "✅ Активен" : "❌ Ошибка");
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/info" || command == "info") {
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
      
    } else if (command == "/mode" || command == "mode") {
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
      
    } else if (command == "/wifi" || command == "wifi") {
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
      
    } else if (command == "/mqtt" || command == "mqtt") {
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
      
    } else if (command.startsWith("/mode_local") || command == "mode_local") {
      setOperationMode(MODE_LOCAL);
      String message = "✅ *Режим изменен*\n\n";
      message += "📌 *Новый режим:* Локальный\n";
      message += "📝 *Описание:* Только локальный мониторинг, WiFi только при нажатии кнопки";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command.startsWith("/mode_monitoring") || command == "mode_monitoring") {
      setOperationMode(MODE_MONITORING);
      String message = "✅ *Режим изменен*\n\n";
      message += "📌 *Новый режим:* Мониторинг\n";
      message += "📝 *Описание:* Мониторинг с отправкой в MQTT и Telegram";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command.startsWith("/mode_alert") || command == "mode_alert") {
      setOperationMode(MODE_ALERT);
      String message = "✅ *Режим изменен*\n\n";
      message += "📌 *Новый режим:* Оповещение\n";
      message += "📝 *Описание:* Режим оповещения при превышении порогов";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command.startsWith("/mode_stabilization") || command == "mode_stabilization") {
      setOperationMode(MODE_STABILIZATION);
      String message = "✅ *Режим изменен*\n\n";
      message += "📌 *Новый режим:* Стабилизация\n";
      message += "📝 *Описание:* Режим стабилизации температуры\n\n";
      message += "💡 Используйте `/stab_set` для настройки параметров";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command.startsWith("/alert_set") || command == "alert_set") {
      // Парсинг команды: /alert_set <min> <max> [buzzer]
      // Используем оригинальный text для получения параметров (после удаления @botname)
      int firstSpace = text.indexOf(' ');
      if (firstSpace == -1) {
        String message = "❌ *Ошибка формата*\n\n";
        message += "Использование: `/alert_set <min> <max> [buzzer]`\n";
        message += "Пример: `/alert_set 10 30 1`\n";
        message += "buzzer: 1 - включен, 0 - выключен (по умолчанию 1)";
        sendTelegramMessageToQueue(chat_id, message);
      } else {
        String params = text.substring(firstSpace + 1);
        int secondSpace = params.indexOf(' ');
        int thirdSpace = params.indexOf(' ', secondSpace + 1);
        
        if (secondSpace == -1) {
          String message = "❌ *Ошибка формата*\n\n";
          message += "Использование: `/alert_set <min> <max> [buzzer]`";
          sendTelegramMessageToQueue(chat_id, message);
        } else {
          float minTemp = params.substring(0, secondSpace).toFloat();
          float maxTemp = params.substring(secondSpace + 1, thirdSpace > 0 ? thirdSpace : params.length()).toFloat();
          bool buzzerEnabled = true;
          
          if (thirdSpace > 0) {
            String buzzerStr = params.substring(thirdSpace + 1);
            buzzerEnabled = (buzzerStr.toInt() == 1);
          }
          
          if (minTemp >= maxTemp) {
            String message = "❌ *Ошибка*\n\n";
            message += "Минимальная температура должна быть меньше максимальной!";
            sendTelegramMessageToQueue(chat_id, message);
          } else {
            setAlertSettings(minTemp, maxTemp, buzzerEnabled);
            String message = "✅ *Настройки оповещения обновлены*\n\n";
            message += "🔔 *Минимальная температура:* " + String(minTemp, 1) + "°C\n";
            message += "🔔 *Максимальная температура:* " + String(maxTemp, 1) + "°C\n";
            message += "🔊 *Зуммер:* " + String(buzzerEnabled ? "✅ Включен" : "❌ Выключен");
            sendTelegramMessageToQueue(chat_id, message);
          }
        }
      }
      
    } else if (command == "/alert_get" || command == "alert_get") {
      AlertModeSettings alert = getAlertSettings();
      String message = "🔔 *Настройки оповещения*\n\n";
      message += "📉 *Минимальная температура:* " + String(alert.minTemp, 1) + "°C\n";
      message += "📈 *Максимальная температура:* " + String(alert.maxTemp, 1) + "°C\n";
      message += "🔊 *Зуммер:* " + String(alert.buzzerEnabled ? "✅ Включен" : "❌ Выключен");
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command.startsWith("/stab_set") || command == "stab_set") {
      // Парсинг команды: /stab_set <target> [tolerance] [alert] [duration]
      // Используем оригинальный text для получения параметров (после удаления @botname)
      int firstSpace = text.indexOf(' ');
      if (firstSpace == -1) {
        String message = "❌ *Ошибка формата*\n\n";
        message += "Использование: `/stab_set <target> [tolerance] [alert] [duration]`\n";
        message += "Пример: `/stab_set 25 0.1 0.2 600`\n\n";
        message += "Параметры:\n";
        message += "  target - целевая температура (°C)\n";
        message += "  tolerance - допуск (по умолчанию 0.1°C)\n";
        message += "  alert - порог тревоги (по умолчанию 0.2°C)\n";
        message += "  duration - длительность в секундах (по умолчанию 600)";
        sendTelegramMessageToQueue(chat_id, message);
      } else {
        String params = text.substring(firstSpace + 1);
        int spaces[4] = {-1, -1, -1, -1};
        int spaceCount = 0;
        for (int i = 0; i < params.length() && spaceCount < 3; i++) {
          if (params.charAt(i) == ' ') {
            spaces[spaceCount] = i;
            spaceCount++;
          }
        }
        
        float targetTemp = params.substring(0, spaces[0] > 0 ? spaces[0] : params.length()).toFloat();
        float tolerance = 0.1;
        float alertThreshold = 0.2;
        unsigned long duration = 600;
        
        if (spaces[0] > 0) {
          tolerance = params.substring(spaces[0] + 1, spaces[1] > 0 ? spaces[1] : params.length()).toFloat();
        }
        if (spaces[1] > 0) {
          alertThreshold = params.substring(spaces[1] + 1, spaces[2] > 0 ? spaces[2] : params.length()).toFloat();
        }
        if (spaces[2] > 0) {
          duration = params.substring(spaces[2] + 1).toInt();
        }
        
        if (targetTemp <= 0 || tolerance <= 0 || alertThreshold <= 0 || duration <= 0) {
          String message = "❌ *Ошибка*\n\n";
          message += "Все параметры должны быть положительными числами!";
          sendTelegramMessageToQueue(chat_id, message);
        } else {
          setStabilizationSettings(targetTemp, tolerance, alertThreshold, duration);
          String message = "✅ *Настройки стабилизации обновлены*\n\n";
          message += "🎯 *Целевая температура:* " + String(targetTemp, 1) + "°C\n";
          message += "📏 *Допуск:* ±" + String(tolerance, 2) + "°C\n";
          message += "⚠️ *Порог тревоги:* " + String(alertThreshold, 2) + "°C\n";
          message += "⏱️ *Длительность:* " + String(duration) + "с (" + String(duration / 60) + " мин)";
          sendTelegramMessageToQueue(chat_id, message);
        }
      }
      
    } else if (command == "/stab_get" || command == "stab_get") {
      StabilizationModeSettings stab = getStabilizationSettings();
      String message = "🎯 *Настройки стабилизации*\n\n";
      message += "📌 *Целевая температура:* " + String(stab.targetTemp, 1) + "°C\n";
      message += "📏 *Допуск:* ±" + String(stab.tolerance, 2) + "°C\n";
      message += "⚠️ *Порог тревоги:* " + String(stab.alertThreshold, 2) + "°C\n";
      message += "⏱️ *Длительность:* " + String(stab.duration) + "с (" + String(stab.duration / 60) + " мин)";
      
      if (getOperationMode() == MODE_STABILIZATION) {
        message += "\n\n📊 *Статус стабилизации:*\n";
        message += "   Стабилизировано: " + String(isStabilized() ? "✅ Да" : "❌ Нет") + "\n";
        if (isStabilized()) {
          message += "   Время: " + String(getStabilizationTime()) + "с";
        }
      }
      
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/display_on" || command == "display_on") {
      setDisplayScreen(DISPLAY_TEMP);
      String message = "✅ *Дисплей включен*\n\n";
      message += "📺 Показывается экран с температурой";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/display_off" || command == "display_off") {
      turnOffDisplay();
      String message = "✅ *Дисплей выключен*";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/display_temp" || command == "display_temp") {
      setDisplayScreen(DISPLAY_TEMP);
      String message = "✅ *Экран переключен*\n\n";
      message += "📺 Показывается температура: " + String(currentTemp, 1) + "°C";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/display_info" || command == "display_info") {
      setDisplayScreen(DISPLAY_INFO);
      String message = "✅ *Экран переключен*\n\n";
      message += "📺 Показывается информационный экран";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/buzzer_test" || command == "buzzer_test") {
      buzzerBeep(BUZZER_SHORT_BEEP);
      String message = "✅ *Тест зуммера*\n\n";
      message += "🔊 Зуммер должен был издать короткий сигнал";
      sendTelegramMessageToQueue(chat_id, message);
      
    } else if (command == "/reboot" || command == "reboot") {
      String message = "🔄 *Перезагрузка устройства*\n\n";
      message += "Устройство будет перезагружено через 2 секунды...";
      sendTelegramMessageToQueue(chat_id, message);
      delay(2000); // Даем время на отправку сообщения
      ESP.restart();
      
    } else {
      // Неизвестная команда
      String message = "❓ Неизвестная команда: `" + command + "`\n\n";
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

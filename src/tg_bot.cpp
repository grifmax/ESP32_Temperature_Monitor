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
#include "sensors.h"
#include "display.h"
#include "buzzer.h"
#include "sensor_config.h"

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
  bool inUse;  // Для статического пула
};

// Статический пул сообщений вместо new/delete (предотвращает фрагментацию heap)
#define TELEGRAM_POOL_SIZE 5
static TelegramMessage messagePool[TELEGRAM_POOL_SIZE];
static SemaphoreHandle_t poolMutex = NULL;

// Функции для работы со статическим пулом сообщений
static TelegramMessage* allocateMessage() {
  if (poolMutex == NULL) return nullptr;
  if (xSemaphoreTake(poolMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int i = 0; i < TELEGRAM_POOL_SIZE; i++) {
      if (!messagePool[i].inUse) {
        messagePool[i].inUse = true;
        messagePool[i].chatId = "";
        messagePool[i].message = "";
        messagePool[i].isTestMessage = false;
        xSemaphoreGive(poolMutex);
        return &messagePool[i];
      }
    }
    xSemaphoreGive(poolMutex);
  }
  return nullptr;  // Пул исчерпан
}

static void freeMessage(TelegramMessage* msg) {
  if (msg == nullptr || poolMutex == NULL) return;
  if (xSemaphoreTake(poolMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    msg->chatId = "";
    msg->message = "";
    msg->isTestMessage = false;
    msg->inUse = false;
    xSemaphoreGive(poolMutex);
  }
}

// Очередь для Telegram сообщений
QueueHandle_t telegramQueue = NULL;
bool telegramSendInProgress = false;
unsigned long lastTelegramSendAttempt = 0;
unsigned long lastTelegramSendSuccess = 0;
const unsigned long TELEGRAM_SEND_INTERVAL = 2000; // Минимум 2 секунды между отправками
const unsigned long TELEGRAM_SEND_TIMEOUT = 5000; // Таймаут отправки 5 секунд
int telegramConsecutiveFailures = 0;
const int MAX_TELEGRAM_FAILURES = 3; // После 3 неудач подряд - пауза

// FreeRTOS task handle для Telegram polling
TaskHandle_t telegramTaskHandle = NULL;
volatile bool telegramTaskRunning = false;

// ========== ИНТЕРАКТИВНЫЙ РЕЖИМ НАСТРОЙКИ ==========

// Этапы интерактивного диалога
enum InteractiveStep {
  STEP_NONE = 0,           // Нет активного диалога
  STEP_SELECT_MODE,        // Выбор режима работы
  STEP_ALERT_MIN_TEMP,     // Ввод минимальной температуры
  STEP_ALERT_MAX_TEMP,     // Ввод максимальной температуры
  STEP_ALERT_BUZZER,       // Включение зуммера
  STEP_STAB_TOLERANCE,     // Допуск стабилизации
  STEP_STAB_ALERT,         // Порог тревоги
  STEP_STAB_DURATION       // Длительность
};

// Структура состояния интерактивной сессии
struct InteractiveSession {
  String chatId;                    // ID чата
  InteractiveStep step;             // Текущий этап
  unsigned long lastActivity;       // Время последней активности
  OperationMode selectedMode;       // Выбранный режим
  float alertMinTemp;               // Временные настройки оповещения
  float alertMaxTemp;
  bool alertBuzzer;
  float stabTolerance;              // Временные настройки стабилизации
  float stabAlertThreshold;
  unsigned long stabDuration;
  bool valid;                       // Сессия активна
};

// Пул сессий (статический, без динамической памяти)
#define MAX_INTERACTIVE_SESSIONS 3
static InteractiveSession sessions[MAX_INTERACTIVE_SESSIONS];
static const unsigned long SESSION_TIMEOUT = 300000;  // 5 минут таймаут

// Инициализация интерактивных сессий
static void initInteractiveSessions() {
  for (int i = 0; i < MAX_INTERACTIVE_SESSIONS; i++) {
    sessions[i].valid = false;
    sessions[i].chatId = "";
    sessions[i].step = STEP_NONE;
    sessions[i].lastActivity = 0;
  }
}

// Получение сессии для chat_id
static InteractiveSession* getSession(const String& chatId) {
  unsigned long now = millis();
  for (int i = 0; i < MAX_INTERACTIVE_SESSIONS; i++) {
    if (sessions[i].valid && sessions[i].chatId == chatId) {
      // Проверяем таймаут
      if (now - sessions[i].lastActivity > SESSION_TIMEOUT) {
        sessions[i].valid = false;  // Сессия истекла
        return nullptr;
      }
      sessions[i].lastActivity = now;
      return &sessions[i];
    }
  }
  return nullptr;
}

// Создание новой сессии
static InteractiveSession* createSession(const String& chatId) {
  unsigned long now = millis();

  // Очищаем истекшие сессии
  for (int i = 0; i < MAX_INTERACTIVE_SESSIONS; i++) {
    if (sessions[i].valid && (now - sessions[i].lastActivity > SESSION_TIMEOUT)) {
      sessions[i].valid = false;
    }
  }

  // Ищем свободный слот
  for (int i = 0; i < MAX_INTERACTIVE_SESSIONS; i++) {
    if (!sessions[i].valid) {
      sessions[i].chatId = chatId;
      sessions[i].step = STEP_SELECT_MODE;
      sessions[i].lastActivity = now;
      sessions[i].valid = true;
      // Инициализация значений по умолчанию
      sessions[i].alertMinTemp = 10.0;
      sessions[i].alertMaxTemp = 30.0;
      sessions[i].alertBuzzer = true;
      sessions[i].stabTolerance = 0.1;
      sessions[i].stabAlertThreshold = 0.2;
      sessions[i].stabDuration = 600;
      return &sessions[i];
    }
  }
  return nullptr;  // Нет свободных слотов
}

// Удаление сессии
static void deleteSession(const String& chatId) {
  for (int i = 0; i < MAX_INTERACTIVE_SESSIONS; i++) {
    if (sessions[i].valid && sessions[i].chatId == chatId) {
      sessions[i].valid = false;
      sessions[i].chatId = "";
      sessions[i].step = STEP_NONE;
      break;
    }
  }
}

// Forward declaration
static void sendTelegramMessageToQueue(const String& chatId, const String& message, bool isTest = false);

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
    Serial.println(F("Telegram: Bot not configured"));
    return;
  }
  if (!bot || telegramActiveToken != telegramBotToken) {
    if (bot) {
      delete bot;
    }
    bot = new UniversalTelegramBot(telegramBotToken, secured_client);
    telegramActiveToken = telegramBotToken;
    Serial.println(F("Telegram: Bot initialized"));
  }
  telegramInitialized = true;
}

// Инициализация очереди Telegram сообщений
void initTelegramQueue() {
  // Инициализация мьютекса для пула сообщений
  if (poolMutex == NULL) {
    poolMutex = xSemaphoreCreateMutex();
    if (poolMutex == NULL) {
      Serial.println(F("Failed to create pool mutex"));
    }
  }

  // Инициализация пула сообщений
  for (int i = 0; i < TELEGRAM_POOL_SIZE; i++) {
    messagePool[i].inUse = false;
    messagePool[i].chatId = "";
    messagePool[i].message = "";
    messagePool[i].isTestMessage = false;
  }

  if (telegramQueue == NULL) {
    telegramQueue = xQueueCreate(TELEGRAM_POOL_SIZE, sizeof(TelegramMessage*));
    if (telegramQueue == NULL) {
      Serial.println(F("Failed to create Telegram queue"));
    }
  }

  // Инициализация интерактивных сессий
  initInteractiveSessions();
}

// Вспомогательная функция для отправки сообщения через очередь
static void sendTelegramMessageToQueue(const String& chatId, const String& message, bool isTest) {
  if (telegramQueue == NULL) {
    initTelegramQueue();
    if (telegramQueue == NULL) {
      return;
    }
  }

  // Проверяем, не слишком ли много неудач подряд
  if (telegramConsecutiveFailures >= MAX_TELEGRAM_FAILURES) {
    unsigned long now = millis();
    if (now - lastTelegramSendAttempt < 30000) { // 30 секунд паузы
      return; // Слишком много неудач, не добавляем в очередь
    }
  }

  // Используем статический пул вместо new/delete
  TelegramMessage* msg = allocateMessage();
  if (msg == nullptr) {
    // Пул исчерпан - не логируем, чтобы не засорять Serial
    return;
  }

  msg->chatId = chatId;
  msg->message = message;
  msg->isTestMessage = isTest;

  if (xQueueSend(telegramQueue, &msg, 0) != pdTRUE) {
    freeMessage(msg); // Очередь переполнена - возвращаем в пул
  }
}

// Обработка очереди Telegram сообщений (вызывается из loop())
void processTelegramQueue() {
  // Проверяем, что очередь инициализирована
  if (telegramQueue == NULL) {
    initTelegramQueue();
    if (telegramQueue == NULL) {
      return;
    }
  }

  if (telegramSendInProgress) {
    return; // Уже отправляем сообщение
  }

  TelegramMessage* msg = NULL;
  if (xQueueReceive(telegramQueue, &msg, 0) == pdTRUE) {
    if (msg == NULL) return;

    telegramSendInProgress = true;

    // Проверяем подключение WiFi - критически важно перед любыми DNS запросами
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("Telegram queue: WiFi not connected, skipping message"));
      freeMessage(msg);
      telegramSendInProgress = false;
      return;
    }

    // Дополнительная проверка стабильности WiFi перед DNS запросами
    // Проверяем, что WiFi действительно подключен и стабилен
    static unsigned long lastWiFiCheck = 0;
    unsigned long now = millis();
    if (now - lastWiFiCheck > 1000) { // Проверяем не чаще раза в секунду
      if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
        Serial.println(F("Telegram queue: WiFi unstable, skipping message"));
        freeMessage(msg);
        telegramSendInProgress = false;
        return;
      }
      lastWiFiCheck = now;
    }

    ensureTelegramBot();
    updateTelegramFlags(); // Обновляем флаги перед проверкой
    if (!telegramCanSend) {
      Serial.print(F("Telegram queue: Cannot send - configured="));
      Serial.print(telegramConfigured);
      Serial.print(F(", chatId="));
      Serial.println(telegramChatId.length() > 0 ? telegramChatId : "(empty)");
      freeMessage(msg);
      telegramSendInProgress = false;
      return;
    }

    if (!bot) {
      Serial.println(F("Telegram queue: Bot not initialized"));
      freeMessage(msg);
      telegramSendInProgress = false;
      return;
    }

    // Проверяем интервал между отправками
    now = millis(); // Используем уже объявленную переменную
    if (now - lastTelegramSendAttempt < TELEGRAM_SEND_INTERVAL) {
      // Слишком рано, возвращаем сообщение в очередь
      xQueueSendToFront(telegramQueue, &msg, 0);
      telegramSendInProgress = false;
      return;
    }

    // Проверяем, не слишком ли много неудач подряд
    if (telegramConsecutiveFailures >= MAX_TELEGRAM_FAILURES) {
      // Слишком много неудач, делаем паузу
      if (now - lastTelegramSendAttempt < 30000) { // 30 секунд паузы
        freeMessage(msg);
        telegramSendInProgress = false;
        Serial.println(F("Telegram: Too many failures, pausing"));
        return;
      } else {
        // Пауза прошла, сбрасываем счетчик
        telegramConsecutiveFailures = 0;
      }
    }

    lastTelegramSendAttempt = now;

    // Дополнительная проверка WiFi перед отправкой
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
      Serial.println(F("Telegram: WiFi unstable before send, skipping"));
      freeMessage(msg);
      telegramSendInProgress = false;
      telegramConsecutiveFailures++;
      return;
    }

    Serial.print(F("Telegram: Sending to chat "));
    Serial.print(msg->chatId);
    Serial.print(F(", len: "));
    Serial.println(msg->message.length());

    // Используем Markdown для форматирования сообщений
    String parseMode = "Markdown";
    unsigned long sendStart = millis();

    // Добавляем watchdog feed перед длительной операцией
    yield(); // Даем время другим задачам

    // Проверяем WiFi еще раз перед отправкой (DNS lookup может быть проблемным)
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("Telegram: WiFi disconnected before send, skipping"));
      freeMessage(msg);
      telegramSendInProgress = false;
      telegramConsecutiveFailures++;
      return;
    }

    // Пытаемся отправить с обработкой ошибок DNS
    // Проверяем WiFi еще раз перед отправкой
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("Telegram: WiFi disconnected before send, skipping"));
      freeMessage(msg);
      telegramSendInProgress = false;
      telegramConsecutiveFailures++;
      return;
    }

    bool success = bot->sendMessage(msg->chatId, msg->message, parseMode);

    // Проверяем, не отключился ли WiFi после отправки
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("Telegram: WiFi disconnected after send attempt"));
      success = false;
    }

    unsigned long sendDuration = millis() - sendStart;

    // Проверяем таймаут и прерываем, если слишком долго
    if (sendDuration > TELEGRAM_SEND_TIMEOUT) {
      Serial.print(F("Telegram: Send took "));
      Serial.print(sendDuration);
      Serial.println(F(" ms (slow)"));
      // Если отправка заняла слишком много времени, считаем неудачей
      if (sendDuration > 10000) { // 10 секунд - критический таймаут
        success = false;
        Serial.println(F("Telegram: Critical timeout, marking as failed"));
      }
    }

    yield(); // Даем время после отправки

    if (msg->isTestMessage) {
      if (success) {
        Serial.println(F("Telegram test: OK"));
        telegramConsecutiveFailures = 0;
        lastTelegramSendSuccess = now;
      } else {
        Serial.println(F("Telegram test: FAILED"));
        telegramConsecutiveFailures++;
        // Попробуем отправить без форматирования только один раз
        if (telegramConsecutiveFailures == 1) {
          sendStart = millis();
          success = bot->sendMessage(msg->chatId, msg->message, "");
          sendDuration = millis() - sendStart;
          if (success) {
            Serial.println(F("Telegram test: OK (no format)"));
            telegramConsecutiveFailures = 0;
            lastTelegramSendSuccess = now;
          } else {
            Serial.println(F("Telegram test: Still failed"));
            telegramConsecutiveFailures++;
          }
        }
      }
    } else {
      if (success) {
        Serial.println(F("Telegram: Sent"));
        telegramConsecutiveFailures = 0;
        lastTelegramSendSuccess = now;
      } else {
        Serial.println(F("Telegram: Failed"));
        telegramConsecutiveFailures++;
        // Попробуем отправить без форматирования только один раз
        if (telegramConsecutiveFailures == 1) {
          sendStart = millis();
          success = bot->sendMessage(msg->chatId, msg->message, "");
          sendDuration = millis() - sendStart;
          if (success) {
            Serial.println(F("Telegram: Sent (no format)"));
            telegramConsecutiveFailures = 0;
            lastTelegramSendSuccess = now;
          } else {
            Serial.println(F("Telegram: Still failed"));
            telegramConsecutiveFailures++;
          }
        }
      }
    }

    freeMessage(msg);
    telegramSendInProgress = false;
  }
}

// FreeRTOS задача для обработки Telegram сообщений
// Работает в фоне, не блокирует основной loop()
void telegramTask(void* parameter) {
  Serial.println(F("Telegram task started"));
  telegramTaskRunning = true;

  while (true) {
    // Ждём 5 секунд между проверками
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Проверяем, что WiFi подключен
    if (WiFi.status() != WL_CONNECTED) {
      telegramLastPollOk = false;
      continue;
    }

    // Дополнительная проверка стабильности WiFi
    if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
      telegramLastPollOk = false;
      continue;
    }

    // Обрабатываем входящие сообщения
    handleTelegramMessages();

    // Сбрасываем WDT для этой задачи
    vTaskDelay(pdMS_TO_TICKS(10)); // Короткая пауза для yield

    // Обрабатываем очередь исходящих сообщений
    processTelegramQueue();
  }

  telegramTaskRunning = false;
  vTaskDelete(NULL);
}

void startTelegramBot() {
  // Настройка SSL для Telegram
  // Для ESP32 можно использовать setInsecure() для тестирования
  // В продакшене следует использовать setCACert() с правильным сертификатом
  secured_client.setInsecure(); // Используется для тестирования
  // Устанавливаем таймауты для предотвращения зависаний
  secured_client.setTimeout(5); // 5 секунд таймаут для подключения
  ensureTelegramBot();
  initTelegramQueue(); // Инициализируем очередь

  // Создаём FreeRTOS задачу для Telegram polling
  // Запускаем на ядре 0 (Protocol CPU), чтобы не блокировать основной loop на ядре 1
  if (telegramTaskHandle == NULL) {
    xTaskCreatePinnedToCore(
      telegramTask,         // Функция задачи
      "TelegramTask",       // Имя задачи
      8192,                 // Размер стека (8KB для SSL/HTTPS)
      NULL,                 // Параметр
      1,                    // Приоритет (низкий)
      &telegramTaskHandle,  // Хэндл задачи
      0                     // Ядро 0
    );
    Serial.println(F("Telegram task created on core 0"));
  }

  if (telegramConfigured) {
    Serial.println(F("Telegram bot initialized"));
  } else {
    Serial.println(F("Telegram bot not configured"));
  }
}

// Обработка интерактивного ввода
static bool handleInteractiveInput(const String& chatId, const String& text) {
  InteractiveSession* session = getSession(chatId);
  if (!session || session->step == STEP_NONE) {
    return false;  // Нет активной сессии
  }

  String response;
  int choice = text.toInt();

  switch (session->step) {
    case STEP_SELECT_MODE:
      if (choice < 1 || choice > 4) {
        response = "Неверный выбор. Введите число от 1 до 4:";
        sendTelegramMessageToQueue(chatId, response);
        return true;
      }
      switch (choice) {
        case 1: session->selectedMode = MODE_LOCAL; break;
        case 2: session->selectedMode = MODE_MONITORING; break;
        case 3: session->selectedMode = MODE_ALERT; break;
        case 4: session->selectedMode = MODE_STABILIZATION; break;
      }

      if (session->selectedMode == MODE_ALERT) {
        session->step = STEP_ALERT_MIN_TEMP;
        response = "Режим оповещения выбран.\n\nВведите минимальную температуру (C):";
      } else if (session->selectedMode == MODE_STABILIZATION) {
        session->step = STEP_STAB_TOLERANCE;
        response = "Режим стабилизации выбран.\n\nВведите допуск температуры (C, по умолчанию 0.1):";
      } else {
        // MODE_LOCAL или MODE_MONITORING - сразу применяем
        setOperationMode(session->selectedMode);
        const char* modeNames[] = {"Локальный", "Мониторинг", "Оповещение", "Стабилизация"};
        response = "Настройки сохранены:\n- Режим: " + String(modeNames[session->selectedMode]);
        deleteSession(chatId);
      }
      sendTelegramMessageToQueue(chatId, response);
      return true;

    case STEP_ALERT_MIN_TEMP:
      {
        float temp = text.toFloat();
        if (temp < -55 || temp > 125) {
          response = "Некорректная температура. Введите значение от -55 до 125:";
          sendTelegramMessageToQueue(chatId, response);
          return true;
        }
        session->alertMinTemp = temp;
        session->step = STEP_ALERT_MAX_TEMP;
        response = "Минимальная температура: " + String(temp, 1) + "C\n\nВведите максимальную температуру (C):";
        sendTelegramMessageToQueue(chatId, response);
      }
      return true;

    case STEP_ALERT_MAX_TEMP:
      {
        float temp = text.toFloat();
        if (temp < -55 || temp > 125 || temp <= session->alertMinTemp) {
          response = "Некорректная температура. Должна быть больше минимальной (" +
                     String(session->alertMinTemp, 1) + "C):";
          sendTelegramMessageToQueue(chatId, response);
          return true;
        }
        session->alertMaxTemp = temp;
        session->step = STEP_ALERT_BUZZER;
        response = "Максимальная температура: " + String(temp, 1) + "C\n\nВключить зуммер?\n1. Да\n2. Нет";
        sendTelegramMessageToQueue(chatId, response);
      }
      return true;

    case STEP_ALERT_BUZZER:
      if (choice != 1 && choice != 2) {
        response = "Введите 1 (Да) или 2 (Нет):";
        sendTelegramMessageToQueue(chatId, response);
        return true;
      }
      session->alertBuzzer = (choice == 1);

      // Применяем настройки
      setOperationMode(MODE_ALERT);
      setAlertSettings(session->alertMinTemp, session->alertMaxTemp, session->alertBuzzer);

      response = "Настройки сохранены:\n";
      response += "- Режим: Оповещение\n";
      response += "- Мин. температура: " + String(session->alertMinTemp, 1) + "C\n";
      response += "- Макс. температура: " + String(session->alertMaxTemp, 1) + "C\n";
      response += "- Зуммер: " + String(session->alertBuzzer ? "Включен" : "Выключен");
      deleteSession(chatId);
      sendTelegramMessageToQueue(chatId, response);
      return true;

    case STEP_STAB_TOLERANCE:
      {
        float tol = text.toFloat();
        if (tol < 0.1 || tol > 10) {
          response = "Некорректное значение. Введите допуск от 0.1 до 10:";
          sendTelegramMessageToQueue(chatId, response);
          return true;
        }
        session->stabTolerance = tol;
        session->step = STEP_STAB_ALERT;
        response = "Допуск: " + String(tol, 2) + "C\n\nВведите порог тревоги (C, по умолчанию 0.2):";
        sendTelegramMessageToQueue(chatId, response);
      }
      return true;

    case STEP_STAB_ALERT:
      {
        float alert = text.toFloat();
        if (alert < 0.1 || alert > 20) {
          response = "Некорректное значение. Введите от 0.1 до 20:";
          sendTelegramMessageToQueue(chatId, response);
          return true;
        }
        session->stabAlertThreshold = alert;
        session->step = STEP_STAB_DURATION;
        response = "Порог тревоги: " + String(alert, 2) + "C\n\n";
        response += "Введите время стабилизации в секундах (по умолчанию 600 = 10 минут):";
        sendTelegramMessageToQueue(chatId, response);
      }
      return true;

    case STEP_STAB_DURATION:
      {
        unsigned long dur = text.toInt();
        if (dur < 1 || dur > 3600) {
          response = "Некорректное значение. Введите от 1 до 3600 секунд:";
          sendTelegramMessageToQueue(chatId, response);
          return true;
        }
        session->stabDuration = dur;

        // Применяем настройки
        setOperationMode(MODE_STABILIZATION);
        setStabilizationSettings(session->stabTolerance, session->stabAlertThreshold, session->stabDuration);

        response = "Настройки сохранены:\n";
        response += "- Режим: Стабилизация\n";
        response += "- Допуск: " + String(session->stabTolerance, 2) + "C\n";
        response += "- Порог тревоги: " + String(session->stabAlertThreshold, 2) + "C\n";
        response += "- Время: " + String(session->stabDuration) + " сек (" + String(session->stabDuration / 60) + " мин)";
        deleteSession(chatId);
        sendTelegramMessageToQueue(chatId, response);
      }
      return true;

    default:
      deleteSession(chatId);
      return false;
  }
}

void handleTelegramMessages() {
  // Проверяем WiFi перед любыми операциями с Telegram
  if (WiFi.status() != WL_CONNECTED) {
    telegramLastPollOk = false;
    return;
  }

  // Дополнительная проверка стабильности WiFi
  if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    telegramLastPollOk = false;
    return;
  }

  ensureTelegramBot();
  if (!bot || !telegramConfigured) {
    return;
  }

  // Проверяем WiFi еще раз перед getUpdates (может быть DNS lookup)
  if (WiFi.status() != WL_CONNECTED) {
    telegramLastPollOk = false;
    return;
  }

  // Используем offset для получения только новых сообщений
  // last_message_received содержит ID последнего обработанного сообщения
  // Передаем last_message_received + 1, чтобы получить только новые сообщения
  int numNewMessages = -1;
  // Проверяем WiFi еще раз перед getUpdates
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    numNewMessages = bot->getUpdates(bot->last_message_received + 1);
  } else {
    Serial.println(F("Telegram: WiFi unstable, skipping getUpdates"));
    numNewMessages = -1;
  }
  
  // Проверяем, не отключился ли WiFi после getUpdates
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Telegram: WiFi disconnected after getUpdates"));
    telegramLastPollOk = false;
    return;
  }
  
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

    // Сначала проверяем интерактивный ввод (если есть активная сессия)
    if (handleInteractiveInput(chat_id, originalText)) {
      continue;  // Сообщение обработано интерактивным режимом
    }

    if (command == "/setup" || command == "setup") {
      // Запуск интерактивного режима настройки
      InteractiveSession* session = createSession(chat_id);
      if (session) {
        String message = "⚙️ *Интерактивная настройка*\n\n";
        message += "Выберите режим работы:\n\n";
        message += "1️⃣ Локальный - только мониторинг\n";
        message += "2️⃣ Мониторинг - с отправкой в MQTT/Telegram\n";
        message += "3️⃣ Оповещение - при превышении порогов\n";
        message += "4️⃣ Стабилизация - контроль температуры\n\n";
        message += "Введите номер (1-4) или /cancel для отмены:";
        sendTelegramMessageToQueue(chat_id, message);
      } else {
        String message = "❌ *Ошибка*\n\n";
        message += "Слишком много активных сессий. Попробуйте позже.";
        sendTelegramMessageToQueue(chat_id, message);
      }

    } else if (command == "/cancel" || command == "cancel") {
      // Отмена интерактивного режима
      InteractiveSession* session = getSession(chat_id);
      if (session) {
        deleteSession(chat_id);
        String message = "❌ *Настройка отменена*\n\n";
        message += "Интерактивный режим завершен.";
        sendTelegramMessageToQueue(chat_id, message);
      } else {
        String message = "ℹ️ Нет активной сессии настройки.";
        sendTelegramMessageToQueue(chat_id, message);
      }

    } else if (command == "/start" || command == "/help" || command == "help" || command == "start") {
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
      message += "⚙️ *Интерактивная настройка:*\n";
      message += "🔹 `/setup` - пошаговая настройка режимов\n";
      message += "🔹 `/cancel` - отмена настройки\n\n";
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
      message += "🔹 `/stab_set [tolerance] [alert] [duration]`\n";
      message += "   Пример: `/stab_set 0.1 0.2 600`\n";
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
      String message = "📊 *Статус устройства*\n\n";
      
      // Информация о термометрах
      int sensorCount = getSensorCount();
      message += "🌡️ *Термометры:* " + String(sensorCount) + "\n\n";
      
      // Загружаем настройки термометров для получения имен и режимов
      String settingsJson = getSettings();
      StaticJsonDocument<4096> doc;
      DeserializationError error = deserializeJson(doc, settingsJson);
      
      if (!error && doc["sensors"].is<JsonArray>()) {
        JsonArray sensors = doc["sensors"].as<JsonArray>();
        
        // Создаем карту настроек по адресу
        StaticJsonDocument<2048> sensorsMapDoc;
        JsonObject sensorsMap = sensorsMapDoc.to<JsonObject>();
        for (JsonObject sensor : sensors) {
          String addr = sensor["address"].as<String>();
          if (addr.length() > 0) {
            sensorsMap[addr] = sensor;
          }
        }
        
        // Выводим информацию о каждом термометре
        for (int i = 0; i < sensorCount; i++) {
          String addressStr = getSensorAddressString(i);
          float temp = getSensorTemperature(i);
          
          message += "🌡️ *Термометр " + String(i + 1) + "*\n";
          
          // Ищем настройки по адресу
          if (sensorsMap[addressStr].is<JsonObject>()) {
            JsonObject sensorSettings = sensorsMap[addressStr];
            String name = sensorSettings["name"].as<String>();
            if (name.length() == 0) {
              name = "Термометр " + String(i + 1);
            }
            String mode = sensorSettings["mode"].as<String>();
            if (mode.length() == 0) {
              mode = "monitoring";
            }
            bool enabled = sensorSettings["enabled"] | true;
            
            message += "   📝 *Имя:* " + name + "\n";
            message += "   ⚙️ *Режим:* ";
            if (mode == "monitoring") {
              message += "Мониторинг\n";
            } else if (mode == "alert") {
              message += "Оповещение\n";
            } else if (mode == "stabilization") {
              message += "Стабилизация\n";
            } else {
              message += mode + "\n";
            }
            message += "   ✅ *Статус:* " + String(enabled ? "Включен" : "Выключен") + "\n";
          } else {
            message += "   📝 *Имя:* Термометр " + String(i + 1) + "\n";
            message += "   ⚙️ *Режим:* Мониторинг\n";
            message += "   ✅ *Статус:* Включен\n";
          }
          
          message += "   🌡️ *Температура:* " + String(temp != -127.0 ? String(temp, 1) : "Ошибка") + "°C\n";
          message += "   🔗 *Адрес:* `" + addressStr + "`\n\n";
        }
      } else {
        // Если не удалось загрузить настройки, показываем базовую информацию
        for (int i = 0; i < sensorCount; i++) {
          String addressStr = getSensorAddressString(i);
          float temp = getSensorTemperature(i);
          message += "🌡️ *Термометр " + String(i + 1) + "*\n";
          message += "   🌡️ *Температура:* " + String(temp != -127.0 ? String(temp, 1) : "Ошибка") + "°C\n";
          message += "   🔗 *Адрес:* `" + addressStr + "`\n\n";
        }
      }
      
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
        message += "   Допуск: ±" + String(stab.tolerance, 2) + "°C\n";
        message += "   Порог тревоги: " + String(stab.alertThreshold, 2) + "°C\n";
        message += "   Длительность: " + String(stab.duration) + "с (" + String(stab.duration / 60) + " мин)";
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
      // Парсинг команды: /stab_set [tolerance] [alert] [duration]
      // targetTemp убран - используется per-sensor stabTargetTemp
      int firstSpace = text.indexOf(' ');

      // Значения по умолчанию
      float tolerance = 0.1;
      float alertThreshold = 0.2;
      unsigned long duration = 600;

      if (firstSpace == -1) {
        // Без параметров - используем значения по умолчанию
        setStabilizationSettings(tolerance, alertThreshold, duration);
        String message = "✅ *Настройки стабилизации (по умолчанию)*\n\n";
        message += "📏 *Допуск:* ±" + String(tolerance, 2) + "°C\n";
        message += "⚠️ *Порог тревоги:* " + String(alertThreshold, 2) + "°C\n";
        message += "⏱️ *Длительность:* " + String(duration) + "с (" + String(duration / 60) + " мин)\n\n";
        message += "💡 Использование: `/stab_set [tolerance] [alert] [duration]`\n";
        message += "   Пример: `/stab_set 0.1 0.2 600`";
        sendTelegramMessageToQueue(chat_id, message);
      } else {
        String params = text.substring(firstSpace + 1);
        int spaces[3] = {-1, -1, -1};
        int spaceCount = 0;
        for (unsigned int i = 0; i < params.length() && spaceCount < 2; i++) {
          if (params.charAt(i) == ' ') {
            spaces[spaceCount] = i;
            spaceCount++;
          }
        }

        // Первый параметр - tolerance
        tolerance = params.substring(0, spaces[0] > 0 ? spaces[0] : params.length()).toFloat();

        // Второй параметр - alertThreshold (если есть)
        if (spaces[0] > 0) {
          alertThreshold = params.substring(spaces[0] + 1, spaces[1] > 0 ? spaces[1] : params.length()).toFloat();
        }

        // Третий параметр - duration (если есть)
        if (spaces[1] > 0) {
          duration = params.substring(spaces[1] + 1).toInt();
        }

        if (tolerance <= 0 || alertThreshold <= 0 || duration <= 0) {
          String message = "❌ *Ошибка*\n\n";
          message += "Все параметры должны быть положительными числами!\n\n";
          message += "Использование: `/stab_set [tolerance] [alert] [duration]`\n";
          message += "Пример: `/stab_set 0.1 0.2 600`";
          sendTelegramMessageToQueue(chat_id, message);
        } else {
          setStabilizationSettings(tolerance, alertThreshold, duration);
          String message = "✅ *Настройки стабилизации обновлены*\n\n";
          message += "📏 *Допуск:* ±" + String(tolerance, 2) + "°C\n";
          message += "⚠️ *Порог тревоги:* " + String(alertThreshold, 2) + "°C\n";
          message += "⏱️ *Длительность:* " + String(duration) + "с (" + String(duration / 60) + " мин)";
          sendTelegramMessageToQueue(chat_id, message);
        }
      }
      
    } else if (command == "/stab_get" || command == "stab_get") {
      StabilizationModeSettings stab = getStabilizationSettings();
      String message = "🎯 *Настройки стабилизации*\n\n";
      message += "📏 *Допуск:* ±" + String(stab.tolerance, 2) + "°C\n";
      message += "⚠️ *Порог тревоги:* " + String(stab.alertThreshold, 2) + "°C\n";
      message += "⏱️ *Длительность:* " + String(stab.duration) + "с (" + String(stab.duration / 60) + " мин)\n\n";
      message += "💡 Целевая температура задается для каждого термометра отдельно.";
      
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
  sendMetricsToTelegram("", currentTemp);
}

void sendMetricsToTelegram(const String& sensorName, float temperature) {
  if (WiFi.status() != WL_CONNECTED) {
    return; // WiFi не подключен
  }
  
  ensureTelegramBot();
  updateTelegramFlags();
  
  if (!telegramCanSend) {
    return; // Telegram не настроен
  }
  
  // Проверяем, не слишком ли много неудач подряд
  if (telegramConsecutiveFailures >= MAX_TELEGRAM_FAILURES) {
    unsigned long now = millis();
    if (now - lastTelegramSendAttempt < 30000) { // 30 секунд паузы
      return; // Слишком много неудач, пропускаем
    }
  }
  
  unsigned long hours = deviceUptime / 3600;
  unsigned long minutes = (deviceUptime % 3600) / 60;
  
  String message = "📊 *Метрики устройства*\n\n";
  
  // Если указано имя термометра, отправляем только его
  if (sensorName.length() > 0) {
    message += "🌡️ " + sensorName + ": " + String(temperature, 1) + "°C\n";
  } else {
    // Если имя не указано, собираем все термометры
    // Используем кеш настроек из main.cpp вместо загрузки из файла каждый раз
    int sensorCount = getSensorCount();
    if (sensorCount > 0) {
      // Добавляем информацию о каждом термометре из кеша
      for (int i = 0; i < sensorCount; i++) {
        String addressStr = getSensorAddressString(i);
        float temp = getSensorTemperature(i);
        
        if (temp == -127.0) {
          continue; // Пропускаем невалидные температуры
        }
        
        // Ищем настройки в кеше
        String name = "Термометр " + String(i + 1);
        float correction = 0.0;
        bool enabled = true;
        
        for (int j = 0; j < sensorConfigCount && j < MAX_SENSORS; j++) {
          if (sensorConfigs[j].valid && sensorConfigs[j].address == addressStr) {
            name = sensorConfigs[j].name;
            correction = sensorConfigs[j].correction;
            enabled = sensorConfigs[j].enabled;
            break;
          }
        }
        
        if (!enabled) {
          continue; // Пропускаем выключенные термометры
        }
        
        // Применяем коррекцию
        float correctedTemp = temp + correction;
        message += "🌡️ " + name + ": " + String(correctedTemp, 1) + "°C\n";
        
        yield(); // Даем время другим задачам
      }
    } else {
      // Если термометров нет, используем старую логику
      message += "🌡️ Температура: " + String(temperature, 1) + "°C\n";
    }
  }
  
  message += "\n🌐 IP: " + deviceIP + "\n";
  message += "⏱️ Время работы: " + String(hours) + "ч " + String(minutes) + "м\n";
  message += "📶 Wi-Fi RSSI: " + String(wifiRSSI) + " dBm";
  
  // Используем очередь для отправки метрик
  sendTelegramMessageToQueue(telegramChatId, message);
}

void sendTemperatureAlert(float temperature) {
  sendTemperatureAlert("", temperature, "");
}

void sendTemperatureAlert(const String& sensorName, float temperature, const String& alertType) {
  if (WiFi.status() != WL_CONNECTED) {
    return; // WiFi не подключен
  }
  
  ensureTelegramBot();
  updateTelegramFlags();
  
  if (!telegramCanSend) {
    return; // Telegram не настроен
  }
  
  // Проверяем, не слишком ли много неудач подряд
  if (telegramConsecutiveFailures >= MAX_TELEGRAM_FAILURES) {
    unsigned long now = millis();
    if (now - lastTelegramSendAttempt < 30000) { // 30 секунд паузы
      return; // Слишком много неудач, пропускаем
    }
  }
  
  String alert = "⚠️ *Температурное оповещение*\n\n";
  if (sensorName.length() > 0) {
    alert += "🌡️ " + sensorName + "\n";
  }
  
  if (alertType.length() > 0) {
    if (alertType == "high") {
      alert += "🔥 *Высокая температура!*\n";
    } else if (alertType == "low") {
      alert += "❄️ *Низкая температура!*\n";
    } else {
      alert += alertType + "\n";
    }
  } else {
    // Старая логика для обратной совместимости
    if (temperature >= HIGH_TEMP_THRESHOLD) {
      alert += "🔥 *Высокая температура!*\n";
    } else if (temperature <= LOW_TEMP_THRESHOLD) {
      alert += "❄️ *Низкая температура!*\n";
    }
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
  updateTelegramFlags();
  ensureTelegramBot();
  
  Serial.print(F("Telegram config set: token="));
  Serial.print(telegramBotToken.length() > 0 ? "***" : "(empty)");
  Serial.print(F(", chatId="));
  Serial.print(telegramChatId.length() > 0 ? telegramChatId : "(empty)");
  Serial.print(F(", configured="));
  Serial.print(telegramConfigured);
  Serial.print(F(", canSend="));
  Serial.println(telegramCanSend);
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

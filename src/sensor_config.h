#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include <Arduino.h>

// Максимальное количество поддерживаемых датчиков
#define MAX_SENSORS 10

// Размер кольцевого буфера для истории температур (для режима стабилизации)
// При измерении раз в секунду: 120 = 2 минуты истории для анализа скорости изменения
#define STAB_HISTORY_SIZE 120

// Структура конфигурации датчика температуры
struct SensorConfig {
  String address;           // Адрес датчика (hex строка)
  String name;              // Пользовательское имя
  bool enabled;             // Включен ли датчик
  float correction;         // Коррекция температуры (-10..+10)
  String mode;              // Режим: "monitoring", "alert", "stabilization"
  bool sendToNetworks;      // Отправлять в MQTT/Telegram
  bool buzzerEnabled;       // Бипер для этого датчика
  float alertMinTemp;       // Минимальный порог оповещения (-55..+125)
  float alertMaxTemp;       // Максимальный порог оповещения (-55..+125)
  bool alertBuzzerEnabled;  // Бипер при срабатывании оповещения
  float stabTolerance;      // Допуск стабилизации (0.1..10) - макс. разброс температур за duration
  float stabAlertThreshold; // Порог тревоги - резкий скачок от базовой температуры (0.1..20)
  unsigned long stabDuration;       // Время ожидания стабилизации (мс) - период анализа стабильности
  unsigned long monitoringInterval; // Интервал мониторинга (1..3600 сек)
  bool stabBuzzerEnabled;   // Бипер при тревоге стабилизации
  bool valid;               // Флаг валидности конфигурации
};

// Структура состояния датчика (для отслеживания)
struct SensorState {
  float lastSentTemp;                   // Последняя отправленная температура
  unsigned long stabilizationStartTime; // Время начала отсчёта стабилизации
  bool isStabilized;                    // Флаг: температура стабилизировалась

  // Данные для режима стабилизации (отслеживание резких скачков)
  float baselineTemp;                   // Базовая температура после стабилизации
  float tempHistory[STAB_HISTORY_SIZE]; // Кольцевой буфер температур
  unsigned long timeHistory[STAB_HISTORY_SIZE]; // Временные метки
  int historyIndex;                     // Текущий индекс в буфере
  int historyCount;                     // Количество записей в буфере
  bool alertSent;                       // Флаг: тревога уже отправлена
  unsigned long lastAlertTime;          // Время последней тревоги (для cooldown)
};

// Глобальные переменные (определены в main.cpp)
extern SensorConfig sensorConfigs[MAX_SENSORS];
extern SensorState sensorStates[MAX_SENSORS];
extern int sensorConfigCount;
extern bool forceReloadSettings;

// Функция загрузки конфигурации (определена в main.cpp)
void loadSensorConfigs();

#endif // SENSOR_CONFIG_H

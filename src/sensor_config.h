#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include <Arduino.h>

// Максимальное количество поддерживаемых датчиков
#define MAX_SENSORS 10

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
  float stabTargetTemp;     // Целевая температура стабилизации (-55..+125)
  float stabTolerance;      // Допуск стабилизации (0.1..10)
  float stabAlertThreshold; // Порог тревоги стабилизации (0.1..20)
  unsigned long stabDuration;       // Длительность стабилизации (мс)
  unsigned long monitoringInterval; // Интервал мониторинга (1..3600 сек)
  bool valid;               // Флаг валидности конфигурации
};

// Структура состояния датчика (для отслеживания)
struct SensorState {
  float lastSentTemp;                 // Последняя отправленная температура
  unsigned long stabilizationStartTime; // Время начала стабилизации
  bool isStabilized;                  // Флаг стабилизации
};

// Глобальные переменные (определены в main.cpp)
extern SensorConfig sensorConfigs[MAX_SENSORS];
extern SensorState sensorStates[MAX_SENSORS];
extern int sensorConfigCount;
extern bool forceReloadSettings;

// Функция загрузки конфигурации (определена в main.cpp)
void loadSensorConfigs();

#endif // SENSOR_CONFIG_H

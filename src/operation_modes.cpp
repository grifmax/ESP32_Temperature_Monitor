#include "operation_modes.h"
#include "config.h"
#include <Arduino.h>

// Текущий режим работы
OperationMode currentMode = MODE_LOCAL;

// Настройки режима оповещения
AlertModeSettings alertSettings = {
  .minTemp = 10.0,
  .maxTemp = 30.0,
  .buzzerEnabled = true
};

// Настройки режима стабилизации
// targetTemp убран - используется per-sensor stabTargetTemp
StabilizationModeSettings stabilizationSettings = {
  .tolerance = 0.1,
  .alertThreshold = 0.2,
  .duration = 600  // 10 минут
};

// Состояние стабилизации
struct StabilizationState {
  bool isStabilized;
  unsigned long stabilizationStartTime;
  float lastTemp;
  unsigned long lastChangeTime;
} stabilizationState = {
  .isStabilized = false,
  .stabilizationStartTime = 0,
  .lastTemp = 0.0,
  .lastChangeTime = 0
};

void initOperationModes() {
  currentMode = MODE_LOCAL;
  stabilizationState.isStabilized = false;
  stabilizationState.stabilizationStartTime = 0;
  stabilizationState.lastTemp = 0.0;
  stabilizationState.lastChangeTime = 0;
}

void setOperationMode(OperationMode mode) {
  currentMode = mode;
  // Сброс состояния стабилизации при смене режима
  if (mode != MODE_STABILIZATION) {
    stabilizationState.isStabilized = false;
    stabilizationState.stabilizationStartTime = 0;
  }
}

OperationMode getOperationMode() {
  return currentMode;
}

void setAlertSettings(float minTemp, float maxTemp, bool buzzerEnabled) {
  alertSettings.minTemp = minTemp;
  alertSettings.maxTemp = maxTemp;
  alertSettings.buzzerEnabled = buzzerEnabled;
}

AlertModeSettings getAlertSettings() {
  return alertSettings;
}

void setStabilizationSettings(float tolerance, float alertThreshold, unsigned long duration) {
  // targetTemp убран - используется per-sensor stabTargetTemp
  stabilizationSettings.tolerance = tolerance;
  stabilizationSettings.alertThreshold = alertThreshold;
  stabilizationSettings.duration = duration;
}

StabilizationModeSettings getStabilizationSettings() {
  return stabilizationSettings;
}

void updateOperationMode() {
  // Логика обновления режимов будет вызываться из main.cpp
  // Здесь можно добавить общую логику, если потребуется
}

// Функция проверки стабилизации (устарела - используется per-sensor логика в main.cpp)
// targetTemp убран из глобальных настроек - функция оставлена для обратной совместимости
bool checkStabilization(float currentTemp) {
  // Эта функция больше не используется - per-sensor стабилизация реализована в main.cpp
  // Возвращаем false для безопасности
  (void)currentTemp; // Подавляем предупреждение о неиспользуемом параметре
  return false;
}

// Функция проверки превышения порога тревоги в режиме стабилизации (устарела)
// targetTemp убран из глобальных настроек - функция оставлена для обратной совместимости
bool checkStabilizationAlert(float currentTemp) {
  // Эта функция больше не используется - per-sensor стабилизация реализована в main.cpp
  // Возвращаем false для безопасности
  (void)currentTemp; // Подавляем предупреждение о неиспользуемом параметре
  return false;
}

// Получение состояния стабилизации
bool isStabilized() {
  return stabilizationState.isStabilized;
}

unsigned long getStabilizationTime() {
  if (stabilizationState.isStabilized) {
    return (millis() - stabilizationState.stabilizationStartTime) / 1000;
  }
  return 0;
}

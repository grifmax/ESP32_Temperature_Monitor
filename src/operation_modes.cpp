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
StabilizationModeSettings stabilizationSettings = {
  .targetTemp = 25.0,
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

void setStabilizationSettings(float targetTemp, float tolerance, float alertThreshold, unsigned long duration) {
  stabilizationSettings.targetTemp = targetTemp;
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

// Функция проверки стабилизации (вызывается извне)
bool checkStabilization(float currentTemp) {
  if (currentMode != MODE_STABILIZATION) {
    return false;
  }
  
  float diff = abs(currentTemp - stabilizationSettings.targetTemp);
  
  // Проверка на попадание в допуск
  if (diff <= stabilizationSettings.tolerance) {
    if (!stabilizationState.isStabilized) {
      // Только что стабилизировались
      stabilizationState.isStabilized = true;
      stabilizationState.stabilizationStartTime = millis();
      stabilizationState.lastTemp = currentTemp;
      stabilizationState.lastChangeTime = millis();
    } else {
      // Проверяем, что температура не меняется значительно
      if (abs(currentTemp - stabilizationState.lastTemp) > stabilizationSettings.tolerance) {
        // Температура вышла за допуск
        stabilizationState.isStabilized = false;
        stabilizationState.stabilizationStartTime = 0;
      }
      stabilizationState.lastTemp = currentTemp;
      stabilizationState.lastChangeTime = millis();
    }
  } else {
    // Не в допуске
    stabilizationState.isStabilized = false;
    stabilizationState.stabilizationStartTime = 0;
  }
  
  // Проверка времени работы на уставке
  if (stabilizationState.isStabilized && 
      (millis() - stabilizationState.stabilizationStartTime) >= (stabilizationSettings.duration * 1000)) {
    return true; // Стабилизация завершена успешно
  }
  
  return false;
}

// Функция проверки превышения порога тревоги в режиме стабилизации
bool checkStabilizationAlert(float currentTemp) {
  if (currentMode != MODE_STABILIZATION) {
    return false;
  }
  
  float diff = abs(currentTemp - stabilizationSettings.targetTemp);
  return diff > stabilizationSettings.alertThreshold;
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

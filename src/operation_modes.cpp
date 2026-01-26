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
  .tolerance = 0.1,
  .alertThreshold = 0.2,
  .duration = 600  // 10 минут
};

// Состояние стабилизации (новая логика - отслеживание колебаний)
struct StabilizationState {
  bool isStabilized;                    // Достигнута ли стабилизация
  unsigned long trackingStartTime;      // Начало периода отслеживания
  float minTempInPeriod;                // Минимальная температура за период
  float maxTempInPeriod;                // Максимальная температура за период
  float stabilizedTemp;                 // Температура стабилизации (среднее при достижении)
  float lastTemp;                       // Последняя измеренная температура
  unsigned long lastUpdateTime;         // Время последнего обновления
} stabilizationState = {
  .isStabilized = false,
  .trackingStartTime = 0,
  .minTempInPeriod = 999.0,
  .maxTempInPeriod = -999.0,
  .stabilizedTemp = 0.0,
  .lastTemp = 0.0,
  .lastUpdateTime = 0
};

void initOperationModes() {
  currentMode = MODE_LOCAL;
  stabilizationState.isStabilized = false;
  stabilizationState.trackingStartTime = 0;
  stabilizationState.minTempInPeriod = 999.0;
  stabilizationState.maxTempInPeriod = -999.0;
  stabilizationState.stabilizedTemp = 0.0;
  stabilizationState.lastTemp = 0.0;
  stabilizationState.lastUpdateTime = 0;
}

void setOperationMode(OperationMode mode) {
  currentMode = mode;
  // Сброс состояния стабилизации при смене режима
  if (mode != MODE_STABILIZATION) {
    stabilizationState.isStabilized = false;
    stabilizationState.trackingStartTime = 0;
    stabilizationState.minTempInPeriod = 999.0;
    stabilizationState.maxTempInPeriod = -999.0;
    stabilizationState.stabilizedTemp = 0.0;
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
// Новая логика: отслеживание колебаний температуры за период duration
// Возвращает true когда температура только что стабилизировалась (один раз)
bool checkStabilization(float currentTemp) {
  if (currentMode != MODE_STABILIZATION) {
    return false;
  }

  unsigned long now = millis();
  stabilizationState.lastTemp = currentTemp;
  stabilizationState.lastUpdateTime = now;

  // Если уже стабилизировано - не возвращаем true повторно
  // (тревога обрабатывается в checkStabilizationAlert)
  if (stabilizationState.isStabilized) {
    return false;
  }

  // Фаза отслеживания (tracking)
  // Инициализация периода отслеживания
  if (stabilizationState.trackingStartTime == 0) {
    stabilizationState.trackingStartTime = now;
    stabilizationState.minTempInPeriod = currentTemp;
    stabilizationState.maxTempInPeriod = currentTemp;
    return false;
  }

  // Обновляем min/max за период
  if (currentTemp < stabilizationState.minTempInPeriod) {
    stabilizationState.minTempInPeriod = currentTemp;
  }
  if (currentTemp > stabilizationState.maxTempInPeriod) {
    stabilizationState.maxTempInPeriod = currentTemp;
  }

  // Вычисляем размах колебаний
  float tempRange = stabilizationState.maxTempInPeriod - stabilizationState.minTempInPeriod;

  // Проверяем, прошло ли достаточно времени для определения стабильности
  unsigned long elapsedTime = now - stabilizationState.trackingStartTime;
  unsigned long requiredTime = stabilizationSettings.duration * 1000;

  if (elapsedTime >= requiredTime) {
    // Время прошло - проверяем колебания
    // Колебания должны быть в пределах ±tolerance (т.е. размах <= tolerance * 2)
    if (tempRange <= stabilizationSettings.tolerance * 2) {
      // Стабилизация достигнута!
      stabilizationState.isStabilized = true;
      // Запоминаем среднюю температуру как стабильную
      stabilizationState.stabilizedTemp = (stabilizationState.minTempInPeriod + stabilizationState.maxTempInPeriod) / 2.0;

      Serial.print(F("Stabilization reached! Temp: "));
      Serial.print(stabilizationState.stabilizedTemp);
      Serial.print(F("°C, range: ±"));
      Serial.print(tempRange / 2.0);
      Serial.println(F("°C"));

      return true; // Сигнал о достижении стабилизации (один раз)
    } else {
      // Колебания слишком большие - сбрасываем период и начинаем заново
      stabilizationState.trackingStartTime = now;
      stabilizationState.minTempInPeriod = currentTemp;
      stabilizationState.maxTempInPeriod = currentTemp;
    }
  }

  return false;
}

// Функция проверки превышения порога тревоги в режиме стабилизации
// Возвращает true если температура отклонилась от стабильной более чем на alertThreshold
// При срабатывании тревоги сбрасывает состояние стабилизации и возвращает в режим отслеживания
bool checkStabilizationAlert(float currentTemp) {
  if (currentMode != MODE_STABILIZATION) {
    return false;
  }

  // Тревога только если уже была достигнута стабилизация
  if (!stabilizationState.isStabilized) {
    return false;
  }

  float diff = abs(currentTemp - stabilizationState.stabilizedTemp);

  if (diff > stabilizationSettings.alertThreshold) {
    // Температура отклонилась - тревога!
    Serial.print(F("Stabilization alert! Current: "));
    Serial.print(currentTemp);
    Serial.print(F("°C, stabilized: "));
    Serial.print(stabilizationState.stabilizedTemp);
    Serial.print(F("°C, diff: "));
    Serial.print(diff);
    Serial.println(F("°C"));

    // Сбрасываем состояние - возвращаемся в режим отслеживания
    stabilizationState.isStabilized = false;
    stabilizationState.trackingStartTime = millis();
    stabilizationState.minTempInPeriod = currentTemp;
    stabilizationState.maxTempInPeriod = currentTemp;

    return true;
  }

  return false;
}

// Получение состояния стабилизации
bool isStabilized() {
  return stabilizationState.isStabilized;
}

// Получить температуру стабилизации
float getStabilizedTemp() {
  return stabilizationState.stabilizedTemp;
}

unsigned long getStabilizationTime() {
  if (stabilizationState.trackingStartTime > 0) {
    return (millis() - stabilizationState.trackingStartTime) / 1000;
  }
  return 0;
}

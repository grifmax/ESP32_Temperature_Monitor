#ifndef OPERATION_MODES_H
#define OPERATION_MODES_H

// Режимы работы термометра
enum OperationMode {
  MODE_LOCAL = 0,           // Локальный режим - только мониторинг, WiFi только при нажатии кнопки
  MODE_MONITORING = 1,      // Мониторинг с отправкой в MQTT и Telegram
  MODE_ALERT = 2,           // Режим оповещения при превышении порогов
  MODE_STABILIZATION = 3    // Режим стабилизации температуры
};

// Структура настроек режима оповещения
struct AlertModeSettings {
  float minTemp;      // Минимальная температура срабатывания
  float maxTemp;      // Максимальная температура срабатывания
  bool buzzerEnabled; // Включен ли бипер
};

// Структура настроек режима стабилизации
// targetTemp убран - используется per-sensor stabTargetTemp
struct StabilizationModeSettings {
  float tolerance;         // Допуск стабильности (по умолчанию 0.1°C)
  float alertThreshold;    // Порог тревоги (по умолчанию 0.2°C)
  unsigned long duration;  // Время работы на уставке в секундах (по умолчанию 600 = 10 минут)
};

// Функции управления режимами
void initOperationModes();
void setOperationMode(OperationMode mode);
OperationMode getOperationMode();
void updateOperationMode();

// Функции для режима оповещения
void setAlertSettings(float minTemp, float maxTemp, bool buzzerEnabled);
AlertModeSettings getAlertSettings();

// Функции для режима стабилизации
// targetTemp убран - используется per-sensor stabTargetTemp
void setStabilizationSettings(float tolerance, float alertThreshold, unsigned long duration);
StabilizationModeSettings getStabilizationSettings();

// Вспомогательные функции стабилизации
bool checkStabilization(float currentTemp);
bool checkStabilizationAlert(float currentTemp);
bool isStabilized();
unsigned long getStabilizationTime();

#endif

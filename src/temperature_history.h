#ifndef TEMPERATURE_HISTORY_H
#define TEMPERATURE_HISTORY_H

#include <Arduino.h>

#define MAX_HISTORY_SIZE 288  // 24 часа * 12 записей (1 запись каждые 5 минут) - уменьшено с 1440 для экономии памяти

struct TemperatureRecord {
  unsigned long timestamp;
  float temperature;
  String sensorAddress;  // Адрес термометра для идентификации
};

void initTemperatureHistory();
void addTemperatureRecord(float temp, const String& sensorAddress = "");
TemperatureRecord* getHistory(int* count);
TemperatureRecord* getHistoryForPeriod(unsigned long startTime, unsigned long endTime, int* count);
bool saveHistoryToSPIFFS();
bool loadHistoryFromSPIFFS();

#endif

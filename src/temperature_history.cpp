#include "temperature_history.h"
#include "time_manager.h"
#include <Arduino.h>
#include <stdlib.h>

TemperatureRecord history[MAX_HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;
bool historyInitialized = false;

void initTemperatureHistory() {
  if (!historyInitialized) {
    for (int i = 0; i < MAX_HISTORY_SIZE; i++) {
      history[i].timestamp = 0;
      history[i].temperature = 0.0;
    }
    historyInitialized = true;
  }
}

void addTemperatureRecord(float temp) {
  unsigned long currentTime = getUnixTime();
  
  if (currentTime == 0) {
    // Если время не синхронизировано, используем millis()
    currentTime = millis() / 1000;
  }
  
  // Проверяем, прошло ли 5 минут с последней записи (для уменьшения частоты записей)
  static unsigned long lastRecordTime = 0;
  if (currentTime - lastRecordTime < 300 && historyCount > 0) {
    // Обновляем последнюю запись вместо создания новой
    int lastIndex = (historyIndex - 1 + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
    history[lastIndex].temperature = temp;
    history[lastIndex].timestamp = currentTime;
    return;
  }
  
  // Добавляем новую запись
  history[historyIndex].timestamp = currentTime;
  history[historyIndex].temperature = temp;
  
  historyIndex = (historyIndex + 1) % MAX_HISTORY_SIZE;
  if (historyCount < MAX_HISTORY_SIZE) {
    historyCount++;
  }
  
  lastRecordTime = currentTime;
}

TemperatureRecord* getHistory(int* count) {
  *count = historyCount;
  return history;
}

TemperatureRecord* getHistoryForPeriod(unsigned long startTime, unsigned long endTime, int* count) {
  // Используем оригинальный массив истории вместо создания копии для экономии памяти
  // Подсчитываем количество записей в периоде
  int filteredCount = 0;
  
  // Сначала считаем количество записей
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
    if (history[idx].timestamp >= startTime && history[idx].timestamp <= endTime) {
      filteredCount++;
    }
  }
  
  *count = filteredCount;
  
  // Если записей нет, возвращаем пустой указатель
  if (filteredCount == 0) {
    return nullptr;
  }
  
  // Создаем временный массив только нужного размера (экономия памяти)
  static TemperatureRecord* filtered = nullptr;
  static int filteredSize = 0;
  
  // Перераспределяем память только если нужно больше места
  if (filteredCount > filteredSize) {
    if (filtered != nullptr) {
      free(filtered);
    }
    filtered = (TemperatureRecord*)malloc(filteredCount * sizeof(TemperatureRecord));
    filteredSize = filteredCount;
  }
  
  // Заполняем массив
  int fillIndex = 0;
  for (int i = 0; i < historyCount && fillIndex < filteredCount; i++) {
    int idx = (historyIndex - historyCount + i + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
    if (history[idx].timestamp >= startTime && history[idx].timestamp <= endTime) {
      filtered[fillIndex] = history[idx];
      fillIndex++;
    }
  }
  
  return filtered;
}

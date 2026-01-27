#include "temperature_history.h"
#include "time_manager.h"
#include <Arduino.h>
#include <stdlib.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

TemperatureRecord history[MAX_HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;
bool historyInitialized = false;

void initTemperatureHistory() {
  if (!historyInitialized) {
    for (int i = 0; i < MAX_HISTORY_SIZE; i++) {
      history[i].timestamp = 0;
      history[i].temperature = 0.0;
      history[i].sensorAddress = "";
    }
    historyInitialized = true;
  }
}

void addTemperatureRecord(float temp, const String& sensorAddress) {
  unsigned long currentTime = getUnixTime();
  
  if (currentTime == 0) {
    // Если время не синхронизировано, используем millis()
    currentTime = millis() / 1000;
  }
  
  // Проверяем, прошло ли 5 минут с последней записи (для уменьшения частоты записей)
  static unsigned long lastRecordTime = 0;
  static String lastSensorAddress = "";
  
  // Если прошло меньше 5 минут И это тот же термометр, обновляем последнюю запись
  if (currentTime - lastRecordTime < 300 && historyCount > 0 && 
      sensorAddress.length() > 0 && sensorAddress == lastSensorAddress) {
    // Обновляем последнюю запись вместо создания новой
    int lastIndex = (historyIndex - 1 + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
    history[lastIndex].temperature = temp;
    history[lastIndex].timestamp = currentTime;
    history[lastIndex].sensorAddress = sensorAddress;
    return;
  }
  
  // Добавляем новую запись
  history[historyIndex].timestamp = currentTime;
  history[historyIndex].temperature = temp;
  history[historyIndex].sensorAddress = sensorAddress;
  
  historyIndex = (historyIndex + 1) % MAX_HISTORY_SIZE;
  if (historyCount < MAX_HISTORY_SIZE) {
    historyCount++;
  }
  
  lastRecordTime = currentTime;
  lastSensorAddress = sensorAddress;
  
  // Периодически сохраняем историю в SPIFFS
  saveHistoryToSPIFFS();
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
    // Сначала выделяем новую память, потом освобождаем старую
    TemperatureRecord* newFiltered = (TemperatureRecord*)malloc(filteredCount * sizeof(TemperatureRecord));
    if (newFiltered == nullptr) {
      // Ошибка выделения - НЕ освобождаем старый буфер, он еще может быть полезен
      *count = 0;
      return nullptr;
    }
    // Успешно выделили - теперь можно освободить старый буфер
    if (filtered != nullptr) {
      free(filtered);
    }
    filtered = newFiltered;
    filteredSize = filteredCount;
  }
  
  // Заполняем массив
  int fillIndex = 0;
  for (int i = 0; i < historyCount && fillIndex < filteredCount; i++) {
    int idx = (historyIndex - historyCount + i + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
    // Пропускаем записи с нулевыми или невалидными значениями
    if (history[idx].timestamp >= startTime && 
        history[idx].timestamp <= endTime &&
        history[idx].temperature != 0.0 &&
        history[idx].temperature != -127.0) {
      filtered[fillIndex] = history[idx];
      fillIndex++;
    }
    yield(); // Даем время другим задачам
  }
  
  *count = fillIndex; // Обновляем реальное количество валидных записей
  return filtered;
}

bool saveHistoryToSPIFFS() {
  static unsigned long lastSaveTime = 0;
  // Сохраняем не чаще раза в минуту для экономии ресурсов
  if (millis() - lastSaveTime < 60000 && lastSaveTime > 0) {
    return true; // Пропускаем сохранение, но возвращаем успех
  }
  
  File file = SPIFFS.open("/history.json", "w");
  if (!file) {
    Serial.println(F("Failed to open history file for writing"));
    return false;
  }
  
  StaticJsonDocument<8192> doc;
  JsonArray recordsArray = doc.createNestedArray("records");
  
  // Сохраняем все записи истории
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
    if (history[idx].timestamp > 0 && history[idx].temperature != -127.0) {
      JsonObject record = recordsArray.createNestedObject();
      record["timestamp"] = history[idx].timestamp;
      record["temperature"] = history[idx].temperature;
      record["sensorAddress"] = history[idx].sensorAddress;
    }
    yield(); // Даем время другим задачам
  }
  
  doc["index"] = historyIndex;
  doc["count"] = historyCount;
  
  String output;
  serializeJson(doc, output);
  
  size_t bytesWritten = file.print(output);
  file.flush();
  file.close();
  
  lastSaveTime = millis();
  
  if (bytesWritten > 0) {
    Serial.print(F("History saved to SPIFFS: "));
    Serial.print(historyCount);
    Serial.println(F(" records"));
    return true;
  } else {
    Serial.println(F("Failed to write history to SPIFFS"));
    return false;
  }
}

bool loadHistoryFromSPIFFS() {
  File file = SPIFFS.open("/history.json", "r");
  if (!file) {
    Serial.println(F("History file not found, starting with empty history"));
    return false;
  }
  
  String content = file.readString();
  file.close();
  
  if (content.length() == 0) {
    Serial.println(F("History file is empty"));
    return false;
  }
  
  StaticJsonDocument<8192> doc;
  DeserializationError error = deserializeJson(doc, content);
  
  if (error) {
    Serial.print(F("Failed to parse history JSON: "));
    Serial.println(error.c_str());
    return false;
  }
  
  // Очищаем текущую историю
  for (int i = 0; i < MAX_HISTORY_SIZE; i++) {
    history[i].timestamp = 0;
    history[i].temperature = 0.0;
    history[i].sensorAddress = "";
  }
  
  historyIndex = doc["index"] | 0;
  historyCount = doc["count"] | 0;
  
  // Ограничиваем количество загружаемых записей
  if (historyCount > MAX_HISTORY_SIZE) {
    historyCount = MAX_HISTORY_SIZE;
  }
  
  if (doc.containsKey("records") && doc["records"].is<JsonArray>()) {
    JsonArray recordsArray = doc["records"];
    int loadedCount = 0;
    
    for (JsonObject record : recordsArray) {
      if (loadedCount >= MAX_HISTORY_SIZE) break;
      
      unsigned long ts = record["timestamp"] | 0;
      float temp = record["temperature"] | 0.0;
      String addr = record["sensorAddress"] | "";
      
      // Проверяем валидность данных
      if (ts > 0 && temp != -127.0 && temp != 0.0) {
        int idx = loadedCount % MAX_HISTORY_SIZE;
        history[idx].timestamp = ts;
        history[idx].temperature = temp;
        history[idx].sensorAddress = addr;
        loadedCount++;
      }
      yield(); // Даем время другим задачам
    }
    
    historyCount = loadedCount;
    historyIndex = loadedCount % MAX_HISTORY_SIZE;
    
    Serial.print(F("History loaded from SPIFFS: "));
    Serial.print(historyCount);
    Serial.println(F(" records"));
    return true;
  }
  
  return false;
}

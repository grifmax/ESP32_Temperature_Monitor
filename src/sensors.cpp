#include "sensors.h"
#include <DallasTemperature.h>
#include <OneWire.h>
#include "config.h"
#include <Arduino.h>

extern DallasTemperature sensors;
extern float currentTemp;

// Массив для хранения адресов найденных датчиков
#define MAX_SENSORS 10
static uint8_t sensorAddresses[MAX_SENSORS][8];
static int sensorCount = 0;
static bool sensorsScanned = false;

// Функция преобразования адреса в строку
String addressToString(uint8_t* address) {
  String result = "";
  for (int i = 0; i < 8; i++) {
    if (address[i] < 16) result += "0";
    result += String(address[i], HEX);
    if (i < 7) result += ":";
  }
  result.toUpperCase();
  return result;
}

// Сканирование всех датчиков на шине
void scanSensors() {
  sensorCount = 0;
  sensors.begin();
  
  // Ищем все устройства на шине
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (sensors.getAddress(sensorAddresses[i], i)) {
      sensorCount++;
    } else {
      break;
    }
  }
  
  sensorsScanned = true;
  
  Serial.print(F("Found "));
  Serial.print(sensorCount);
  Serial.println(F(" temperature sensor(s)"));
  
  for (int i = 0; i < sensorCount; i++) {
    Serial.print(F("Sensor "));
    Serial.print(i);
    Serial.print(F(": "));
    Serial.println(addressToString(sensorAddresses[i]));
  }
}

// Получение количества датчиков
int getSensorCount() {
  if (!sensorsScanned) {
    scanSensors();
  }
  return sensorCount;
}

// Получение адреса датчика по индексу
bool getSensorAddress(int index, uint8_t* address) {
  if (index < 0 || index >= sensorCount) {
    return false;
  }
  for (int i = 0; i < 8; i++) {
    address[i] = sensorAddresses[index][i];
  }
  return true;
}

// Получение строкового представления адреса
String getSensorAddressString(int index) {
  if (index < 0 || index >= sensorCount) {
    return "";
  }
  return addressToString(sensorAddresses[index]);
}

// Получение температуры датчика по индексу
float getSensorTemperature(int index) {
  if (index < 0 || index >= sensorCount) {
    return -127.0; // Ошибка чтения
  }
  
  float temp = sensors.getTempC(sensorAddresses[index]);
  if (temp == -127.0 || temp == 85.0) {
    // Ошибка чтения или устройство не отвечает
    return -127.0;
  }
  return temp;
}

// Чтение температуры всех датчиков
void readTemperature() {
  if (!sensorsScanned) {
    scanSensors();
  }
  
  sensors.requestTemperatures();
  
  // Читаем температуру первого датчика для обратной совместимости
  if (sensorCount > 0) {
    currentTemp = getSensorTemperature(0);
  } else {
    currentTemp = -127.0;
  }
}

#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>

// Структура для хранения информации о термометре
struct TemperatureSensor {
  uint8_t address[8];  // 8-байтовый адрес устройства
  float temperature;   // Текущая температура
  bool valid;          // Валидность показаний
  String addressString; // Строковое представление адреса
};

// Функции для работы с датчиками
void readTemperature();
int getSensorCount();
bool getSensorAddress(int index, uint8_t* address);
String getSensorAddressString(int index);
float getSensorTemperature(int index);
void scanSensors(); // Сканирование всех датчиков на шине

#endif

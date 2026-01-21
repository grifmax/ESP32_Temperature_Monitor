#include "sensors.h"
#include <DallasTemperature.h>
#include <OneWire.h>
#include "config.h"

extern DallasTemperature sensors;
extern float currentTemp;

void readTemperature() {
  sensors.requestTemperatures();
  currentTemp = sensors.getTempCByIndex(0);
}

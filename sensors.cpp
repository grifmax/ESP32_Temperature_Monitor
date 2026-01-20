#include "sensors.h"
#include <DallasTemperature.h>
#include <OneWire.h>
#include "config.h"

void readTemperature() {
  sensors.requestTemperatures();
  currentTemp = sensors.getTempCByIndex(0);
}

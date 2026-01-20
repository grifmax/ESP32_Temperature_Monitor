#include "display.h"
#include "config.h"

void updateDisplay() {
  display.clearBuffer();
  display.setCursor(0, 0);
  display.print("Temp: " + String(currentTemp) + "°C");
  display.sendBuffer();
}

#ifndef DISPLAY_H
#define DISPLAY_H

// Режимы экрана
#define DISPLAY_OFF        0
#define DISPLAY_TEMP       1
#define DISPLAY_INFO       2

void updateDisplay();
void showTemperatureScreen(int sensorIndex = -1);
void showInfoScreen();
void turnOffDisplay();
void setDisplayScreen(int screen);
int getCurrentSensorIndex();
void setCurrentSensorIndex(int index);
void nextSensor();

#endif

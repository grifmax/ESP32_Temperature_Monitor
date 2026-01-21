#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <Arduino.h>
#include <time.h>

void initTimeManager();
void updateTime();
String getCurrentTime();
String getCurrentDate();
unsigned long getUnixTime();
void setTimezone(int offset);
int getTimezone();

#endif

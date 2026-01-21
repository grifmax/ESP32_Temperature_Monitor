#ifndef BUZZER_H
#define BUZZER_H

#include <Arduino.h>

// Типы сигналов бипера
enum BuzzerSignal {
  BUZZER_OFF = 0,
  BUZZER_SHORT_BEEP = 1,      // Короткий сигнал
  BUZZER_LONG_BEEP = 2,       // Длинный сигнал
  BUZZER_ALERT = 3,           // Тревожный сигнал (серия коротких)
  BUZZER_STABILIZATION = 4    // Сигнал стабилизации (один длинный)
};

void initBuzzer();
void buzzerBeep(BuzzerSignal signal);
void updateBuzzer(); // Вызывать в loop для обработки сигналов

#endif

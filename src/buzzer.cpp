#include "buzzer.h"
#include "config.h"

unsigned long buzzerStartTime = 0;
unsigned long buzzerDuration = 0;
BuzzerSignal currentBuzzerSignal = BUZZER_OFF;
bool buzzerActive = false;
int beepCount = 0;
int maxBeeps = 0;

void initBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  buzzerActive = false;
}

void buzzerBeep(BuzzerSignal signal) {
  currentBuzzerSignal = signal;
  buzzerStartTime = millis();
  buzzerActive = true;
  beepCount = 0;
  
  switch(signal) {
    case BUZZER_SHORT_BEEP:
      buzzerDuration = 100; // 100 мс
      maxBeeps = 1;
      break;
    case BUZZER_LONG_BEEP:
      buzzerDuration = 500; // 500 мс
      maxBeeps = 1;
      break;
    case BUZZER_ALERT:
      buzzerDuration = 200; // 200 мс на сигнал
      maxBeeps = 3; // 3 сигнала
      break;
    case BUZZER_STABILIZATION:
      buzzerDuration = 1000; // 1 секунда
      maxBeeps = 1;
      break;
    default:
      buzzerActive = false;
      digitalWrite(BUZZER_PIN, LOW);
      return;
  }
  
  // Начать первый сигнал
  digitalWrite(BUZZER_PIN, HIGH);
}

void updateBuzzer() {
  if (!buzzerActive) {
    return;
  }
  
  unsigned long currentTime = millis();
  unsigned long elapsed = currentTime - buzzerStartTime;
  
  if (currentBuzzerSignal == BUZZER_ALERT) {
    // Для тревожного сигнала - серия коротких сигналов
    unsigned long cycleTime = buzzerDuration * 2; // Время одного цикла (сигнал + пауза)
    unsigned long cyclePosition = elapsed % cycleTime;
    int currentCycle = elapsed / cycleTime;
    
    if (currentCycle >= maxBeeps) {
      // Все сигналы завершены
      buzzerActive = false;
      digitalWrite(BUZZER_PIN, LOW);
      return;
    }
    
    if (cyclePosition < buzzerDuration) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }
  } else {
    // Для одиночных сигналов
    if (elapsed < buzzerDuration) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      buzzerActive = false;
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
}

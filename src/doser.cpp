#include "doser.h"
#include <Adafruit_PWMServoDriver.h>

extern Adafruit_PWMServoDriver pwm;

// Массив пинов для удобного обращения к ним в цикле
const int doserPins[4] = {DOSER1_PIN, DOSER2_PIN, DOSER3_PIN, DOSER4_PIN};

// Текстовые метки по умолчанию для удобрений
String doserNames[4] = {"Doser 1", "Doser 2", "Doser 3", "Doser 4"};

unsigned long fertMotorRunTime[4] = {3, 3, 3, 3}; //[cite: 1]
bool fertEnabled[4] = {true, true, true, true};
uint8_t fertTriggerHour[4] = {8, 8, 8, 8}; //[cite: 1]
uint8_t fertTriggerMinute[4] = {0, 0, 0, 0}; //[cite: 1]

// Расписание по дням недели (7 дней)
bool fertDays[4][7] = {
  {true, true, true, true, true, true, true},
  {true, true, true, true, true, true, true},
  {true, true, true, true, true, true, true},
  {true, true, true, true, true, true, true}
}; //[cite: 1]

bool fertMotorActive[4] = {false, false, false, false}; //[cite: 1]
unsigned long fertMotorStartTime[4] = {0, 0, 0, 0}; //[cite: 1]
bool fertTriggered[4] = {false, false, false, false}; //[cite: 1]

void initDosers() {
    for (int i = 0; i < 4; i++) {
        pinMode(doserPins[i], OUTPUT);
        // При старте гарантированно выключаем помпы
        digitalWrite(doserPins[i], LOW); 
    }
}

void handleDosers(int currentHour, int currentMinute, int currentSecond, int weekday) {
    for (int i = 0; i < 4; i++) {
        if (!fertEnabled[i]) {
            if (fertMotorActive[i]) {
                digitalWrite(doserPins[i], LOW);
                fertMotorActive[i] = false;
            }
            fertTriggered[i] = false;
            continue;
        }

        if (fertDays[i][weekday]) { 
            if (!fertMotorActive[i]) { 
                if (currentHour == fertTriggerHour[i] && currentMinute == fertTriggerMinute[i]) { 
                    // Запуск только в первые 5 секунд нужной минуты
                    if (currentSecond < 5 && !fertTriggered[i]) { 
                        digitalWrite(doserPins[i], HIGH); // Открываем транзистор (помпа работает)
                        fertMotorStartTime[i] = millis(); 
                        fertMotorActive[i] = true; 
                        fertTriggered[i] = true; 
                    }
                } else {
                    fertTriggered[i] = false; // Сброс триггера
                }
            } else {
                // Контроль времени работы помпы
                unsigned long runTime = fertMotorRunTime[i];
                if (runTime < 1) runTime = 1;
                if (runTime > 300) runTime = 300;
                unsigned long runTimeMs = runTime * 100;
                if (millis() - fertMotorStartTime[i] >= runTimeMs) { 
                    digitalWrite(doserPins[i], LOW); // Выключаем помпу
                    fertMotorActive[i] = false; 
                }
            }
        } else {
            fertTriggered[i] = false; 
            // Экстренная остановка, если день недели переключился во время работы
            if (fertMotorActive[i]) { 
                unsigned long runTime = fertMotorRunTime[i];
                if (runTime < 1) runTime = 1;
                if (runTime > 300) runTime = 300;
                unsigned long runTimeMs = runTime * 100;
                if (millis() - fertMotorStartTime[i] >= runTimeMs) { 
                    digitalWrite(doserPins[i], LOW); 
                    fertMotorActive[i] = false; 
                }
            }
        }
    }
}
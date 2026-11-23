#ifndef DOSER_H
#define DOSER_H

#include <Arduino.h>

// Задаем прямые цифровые пины ESP32 для управления ключами AO3400
#define DOSER1_PIN 16
#define DOSER2_PIN 18
#define DOSER3_PIN 19
#define DOSER4_PIN 17

// Массив имен для удобрений 
extern String doserNames[4];

// Настройки дозаторов
extern unsigned long fertMotorRunTime[4];
extern bool fertEnabled[4];
extern uint8_t fertTriggerHour[4];
extern uint8_t fertTriggerMinute[4];
extern bool fertDays[4][7];

void initDosers();
void handleDosers(int currentHour, int currentMinute, int currentSecond, int weekday);

#endif // DOSER_H
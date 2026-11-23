#ifndef RELAYS_H
#define RELAYS_H

#include <Arduino.h>

// Задаем прямые цифровые пины ESP32 для управления реле
#define RELAY1_PIN 13
#define RELAY2_PIN 14
#define RELAY3_PIN 26
#define RELAY4_PIN 27

// Массив имен для каналов (будут храниться и изменяться в NVS)
extern String relayNames[4];

// Глобальные настройки реле
extern bool relayState[4];
extern int controlType[4];     // 0 - термостат, 1 - расписание
extern float setPoint[4];      // Целевая температура
extern float hysteresis[4];    // Гистерезис
extern bool schedule[4][48];   // 48 интервалов (по 30 минут)

void initRelays();
void updateRelays(float currentTempWater, int currentHour, int currentMinute);
bool toggleFilterMaintenance();

#endif // RELAYS_H
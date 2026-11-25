#ifndef FEEDER_H
#define FEEDER_H

#include <Arduino.h>

// Пин управления кормушкой на ESP32 (задайте нужный GPIO)
#define FEEDER_PIN 23 

// Настройки кормушки (будут сохраняться в NVS)
extern uint8_t feed1Hour;
extern uint8_t feed1Minute;
extern uint8_t feed2Hour;
extern uint8_t feed2Minute;
extern uint32_t feedDuration; // Длительность прокрутки в мс
extern bool feedFilterPauseEnabled;

// Текущий статус для веб-сервера или других модулей
extern bool isFeeding;

void initFeeder();
void updateFeeder(int currentHour, int currentMinute);
void manualFeed(); // Функция для принудительного кормления (с кнопки или веба)

#endif // FEEDER_H
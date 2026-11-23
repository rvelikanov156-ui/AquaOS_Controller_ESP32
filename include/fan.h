#ifndef FAN_H
#define FAN_H

#include <Arduino.h>

// Глобальные переменные настроек вентилятора (для NVS и Web-сервера)
extern float fanOnTemp;
extern float maxFanTemp;
extern float fanHysteresis;
extern bool isFanOn;
extern int fanStartLevel;

void initFan();
void updateFanLogic();

#endif // FAN_H
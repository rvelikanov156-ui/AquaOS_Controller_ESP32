#ifndef LIGHTING_H
#define LIGHTING_H

#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>

// Ваши каналы на PCA9685
#define CH_R  0
#define CH_B  1
#define CH_W1 2
#define CH_W2 3
#define CH_W3 4

// Настройки яркости
extern int brightnessSettings[5];
extern int moonlightSettings[5];
extern int currentBrightness[5]; 
extern bool isMoonlightEnabled;

// Основные таймеры и рассвет
extern int lightOnHour;
extern int lightOnMinute;
extern int lightOffHour;
extern int lightOffMinute;
extern int sunriseDuration;

// Таймеры дневной паузы
extern int dayPauseStartHour;
extern int dayPauseStartMinute;
extern int dayPauseEndHour;
extern int dayPauseEndMinute;

// Текущий статус света (нужен для веб-сервера и дисплея)
extern bool isLightOn;
// Состояния логики освещения
enum LightState { OFF, MOON, SUNRISE, DAY, PAUSE_SUNSET, PAUSE, PAUSE_SUNRISE, SUNSET };
extern LightState currentLightState;

// Функции
void initLighting();
void setPWMOutputs(const int* brightnessPercent);
void turnOffAllChannels();
void checkLightSchedule(int currentHour, int currentMinute);
void handleSmoothTransition();
void activateMaxBrightness();

#endif // LIGHTING_H
#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

// Расширенный список всех экранов
enum ScreenState {
    SCREEN_MAIN,
    SCREEN_MAIN2,
    SCREEN_MENU_ROOT,
    SCREEN_LIGHT_PWM,
    SCREEN_MOON_PWM,
    SCREEN_LIGHT_SCHED,
    SCREEN_DAY_PAUSE,
    SCREEN_FAN_SET,
    SCREEN_RELAYS,       // Выбор реле и режима
    SCREEN_RELAY_THERM,  // Настройка температуры (если выбран Термостат)
    SCREEN_RELAY_SCHED,  // Настройка таймера (если выбрано Расписание)
    SCREEN_DOSERS,
    SCREEN_FEEDER,
    SCREEN_TIME
};

extern ScreenState currentScreen;

void initDisplay();
void updateDisplay();

#endif // DISPLAY_H
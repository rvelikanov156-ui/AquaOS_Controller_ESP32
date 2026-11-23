#include "lighting.h"

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

const uint8_t pwmChannels[5] = {CH_R, CH_B, CH_W1, CH_W2, CH_W3};

// Эти переменные загружаются из памяти в memory.cpp
int brightnessSettings[5] = {50, 50, 50, 50, 50}; 
int moonlightSettings[5] = {0, 10, 0, 0, 0};
int currentBrightness[5] = {0, 0, 0, 0, 0};      
bool isMoonlightEnabled = true;

int lightOnHour = 8, lightOnMinute = 0;
int lightOffHour = 22, lightOffMinute = 0;
int sunriseDuration = 10; // Общая длительность Рассвета и Заката

int dayPauseStartHour = 25, dayPauseStartMinute = 0; // 25 = отключено
int dayPauseEndHour = 25, dayPauseEndMinute = 0;

// Внутренние рабочие переменные
bool isLightOn = false;
bool maxBrightnessActive = false;
unsigned long maxBrightnessStartedAt = 0;
const unsigned long maxBrightnessDurationMs = 30UL * 60UL * 1000UL;
bool maxBrightnessRestorePending = false;
bool transitionActive = false;

// Состояния логики (что сейчас происходит)
//enum LightState { OFF, MOON, SUNRISE, DAY, PAUSE_SUNSET, PAUSE, PAUSE_SUNRISE, SUNSET };
LightState currentLightState = OFF;

int startBrightnessPWM[5] = {0, 0, 0, 0, 0};
int endBrightnessPWM[5] = {0, 0, 0, 0, 0};
int durationMinutes = 0;

// --- ПЕРЕМЕСТИТЕ ЭТУ ПЕРЕМЕННУЮ СЮДА (УБРАВ ИЗ ВНУТРЕННОСТЕЙ ФУНКЦИИ) ---
int transitionStep = 0;

void initLighting() {
    pwm.begin();
    pwm.setPWMFreq(1000); // Частота ШИМ
    turnOffAllChannels();
}

void setPWMOutputs(const int* brightnessPercent) {
    for (int i = 0; i < 5; i++) {
        currentBrightness[i] = brightnessPercent[i];
        int pwmValue = map(brightnessPercent[i], 0, 100, 0, 4095);
        pwmValue = constrain(pwmValue, 0, 4095);
        pwm.setPWM(pwmChannels[i], 0, pwmValue);
    }
}

void turnOffAllChannels() {
    int off[5] = {0, 0, 0, 0, 0};
    setPWMOutputs(off);
}

// Запуск плавного перехода (mode 1 = Рассвет, 0 = Закат к нулю)
void startTransition(int duration, bool isSunrise) {
    for (int i = 0; i < 5; i++) {
        if (isSunrise) {
            startBrightnessPWM[i] = 0;
            endBrightnessPWM[i] = brightnessSettings[i];
        } else {
            startBrightnessPWM[i] = brightnessSettings[i];
            endBrightnessPWM[i] = 0;
        }
    }
    durationMinutes = duration;
    transitionStep = 0; // СБРОС ШАГА ПРИ КАЖДОМ НОВОМ СТАРТЕ
    transitionActive = true;
}

void handleSmoothTransition() {
    if (!transitionActive) return;

    static unsigned long lastUpdate = 0;
    int totalSteps = durationMinutes * 60; // 1 шаг в секунду

    // 1. ЗАЩИТА ОТ ДЕЛЕНИЯ НА НОЛЬ (Если в настройках Fade стоит 0 минут)
    if (totalSteps <= 0) {
        transitionStep = totalSteps; // Форсируем мгновенное завершение
    }

    // 2. Обработка шагов раз в секунду ИЛИ мгновенно, если Fade = 0
    if (millis() - lastUpdate >= 1000 || totalSteps <= 0) {
        lastUpdate = millis();
        
        // Высчитываем и применяем ШИМ, только если есть плавная длительность
        if (totalSteps > 0) {
            int currentPWM[5];
            for (int i = 0; i < 5; i++) {
                currentPWM[i] = startBrightnessPWM[i] + (endBrightnessPWM[i] - startBrightnessPWM[i]) * transitionStep / totalSteps;
                currentBrightness[i] = currentPWM[i];
                pwm.setPWM(pwmChannels[i], 0, map(currentPWM[i], 0, 100, 0, 4095));
            }
            transitionStep++;
        }

        // 3. Завершение перехода
        if (transitionStep >= totalSteps) {
            transitionStep = 0;
            transitionActive = false;
            
            // Распределяем состояния после завершения
            if (currentLightState == SUNRISE || currentLightState == PAUSE_SUNRISE) {
                currentLightState = DAY;
                setPWMOutputs(brightnessSettings);
                isLightOn = true;
            } else if (currentLightState == SUNSET) {
                isLightOn = false;
                if (isMoonlightEnabled) {
                    currentLightState = MOON;
                    setPWMOutputs(moonlightSettings);
                } else {
                    currentLightState = OFF;
                    turnOffAllChannels();
                }
            } else if (currentLightState == PAUSE_SUNSET) {
                currentLightState = PAUSE;
                turnOffAllChannels();
                isLightOn = false;
            }
        }
    }
}

void checkLightSchedule(int currentHour, int currentMinute) {
    bool maxBrightnessExpired = maxBrightnessRestorePending;
    maxBrightnessRestorePending = false;
    if (maxBrightnessActive) {
        if (millis() - maxBrightnessStartedAt < maxBrightnessDurationMs) return;

        maxBrightnessActive = false;
        maxBrightnessExpired = true;
        transitionActive = false;
        currentLightState = OFF;
    }

    int currentTime = currentHour * 60 + currentMinute;
    int sunriseTime = lightOnHour * 60 + lightOnMinute;
    int sunsetTime = lightOffHour * 60 + lightOffMinute;
    int pauseStart = dayPauseStartHour * 60 + dayPauseStartMinute;
    int pauseEnd = dayPauseEndHour * 60 + dayPauseEndMinute;
    bool hasPause = (dayPauseStartHour != 25 && dayPauseEndHour != 25);

    if (maxBrightnessExpired && currentTime == sunsetTime) {
        currentLightState = DAY;
        setPWMOutputs(brightnessSettings);
        isLightOn = true;
    }

    // 1. Утренний Рассвет (Точно в минуту старта)
    if (currentTime == sunriseTime && currentLightState != SUNRISE && currentLightState != DAY) {
        transitionActive = false; // Сбрасываем любые старые переходы
        turnOffAllChannels(); 
        currentLightState = SUNRISE;
        startTransition(sunriseDuration, true);
        return;
    }

    // 2. Дневная Пауза - Начало
    if (hasPause && currentTime == pauseStart && (currentLightState == DAY || currentLightState == SUNRISE)) {
        transitionActive = false;
        currentLightState = PAUSE;
        turnOffAllChannels(); 
        isLightOn = false;
        return;
    }

    // 3. Дневная Пауза - Окончание
    if (hasPause && currentTime == pauseEnd && currentLightState == PAUSE) {
        if (currentTime >= sunsetTime || currentTime < sunriseTime) {
            isLightOn = false;
            if (isMoonlightEnabled) {
                currentLightState = MOON;
                setPWMOutputs(moonlightSettings);
            } else {
                currentLightState = OFF;
            }
        } else {
            currentLightState = DAY;
            setPWMOutputs(brightnessSettings);
            isLightOn = true;
        }
        return;
    }

    // 4. Вечерний Закат (Точно в минуту старта)
    if (currentTime == sunsetTime && currentLightState == DAY) {
        transitionActive = false;
        currentLightState = SUNSET;
        startTransition(sunriseDuration, false);
        return;
    }

    // 5. Жесткая привязка (Умная защита от сдвигов таймера)
    
    // Вычисляем, сколько минут прошло с момента старта Рассвета и Заката (с учетом полуночи)
    int minsSinceSunrise = currentTime - sunriseTime;
    if (minsSinceSunrise < 0) minsSinceSunrise += 1440;
    
    int minsSinceSunset = currentTime - sunsetTime;
    if (minsSinceSunset < 0) minsSinceSunset += 1440;

    // Если по времени сейчас должен быть ДЕНЬ:
    if (currentTime > sunriseTime && currentTime < sunsetTime) {
        if (hasPause && currentTime >= pauseStart && currentTime < pauseEnd) {
            if (currentLightState != PAUSE) {
                transitionActive = false;
                currentLightState = PAUSE;
                turnOffAllChannels();
                isLightOn = false;
            }
        } else {
            // Если статус Рассвет, но время плавного перехода уже истекло (таймер сдвинули)
            if (currentLightState == SUNRISE && minsSinceSunrise >= sunriseDuration) {
                transitionActive = false;
                currentLightState = DAY;
                setPWMOutputs(brightnessSettings);
                isLightOn = true;
            }
            // Если статус левый (например, зависший Закат или Ночь)
            else if (currentLightState != DAY && currentLightState != SUNRISE) {
                transitionActive = false;
                currentLightState = DAY;
                setPWMOutputs(brightnessSettings);
                isLightOn = true;
            }
        }
    } 
    // Если по времени сейчас должна быть НОЧЬ:
    else if (currentTime < sunriseTime || currentTime > sunsetTime) {
        if (hasPause && currentTime >= pauseStart && currentTime < pauseEnd) {
            if (currentLightState != PAUSE) {
                transitionActive = false;
                currentLightState = PAUSE;
                turnOffAllChannels();
                isLightOn = false;
            }
        } else if (maxBrightnessExpired && currentLightState == OFF) {
            isLightOn = false;
            if (isMoonlightEnabled) {
                currentLightState = MOON;
                setPWMOutputs(moonlightSettings);
            } else {
                turnOffAllChannels();
            }
        } else {
            // Если статус Закат, но время плавного перехода уже истекло (таймер сдвинули в глубокую ночь)
            if (currentLightState == SUNSET && minsSinceSunset >= sunriseDuration) {
                transitionActive = false;
                isLightOn = false;
                if (isMoonlightEnabled) {
                    currentLightState = MOON;
                    setPWMOutputs(moonlightSettings);
                } else {
                    currentLightState = OFF;
                    turnOffAllChannels();
                }
            }
            // Если статус левый (День, Пауза и т.д.)
            else if (currentLightState != MOON && currentLightState != OFF && currentLightState != SUNSET) {
                transitionActive = false;
                isLightOn = false;
                if (isMoonlightEnabled) {
                    currentLightState = MOON;
                    setPWMOutputs(moonlightSettings);
                } else {
                    currentLightState = OFF;
                    turnOffAllChannels();
                }
            }
        }
    }
}

void activateMaxBrightness() {
    if (maxBrightnessActive) {
        maxBrightnessActive = false;
        maxBrightnessRestorePending = true;
        transitionActive = false;
        currentLightState = OFF;

        extern int currentHour;
        extern int currentMinute;
        checkLightSchedule(currentHour, currentMinute);
        return;
    }

    maxBrightnessActive = true;
    maxBrightnessStartedAt = millis();
    transitionActive = false;

    int maxVals[5] = {100, 100, 100, 100, 100};
    setPWMOutputs(maxVals);
    isLightOn = true;
}

#include "fan.h"
#include "temperature.h"    
#include <Adafruit_MCP4725.h>

Adafruit_MCP4725 dac;

// Значения по умолчанию

float fanOnTemp = 30.0;  
float maxFanTemp = 40.0;    
float fanHysteresis = 5.0;  
int fanStartLevel = 30;
bool isFanOn = false;      

// Внутренние переменные для логики и сглаживания
static float currentFanVoltage = 0;  // Значение ЦАП (0-4095)
static unsigned long lastFanOutputUpdate = 0;
static const float fanRampUpPercentPerSecond = 3.0f;
static const float fanRampDownPercentPerSecond = 6.0f;

void initFan() {

    // Инициализация ЦАП (стандартный адрес модуля обычно 0x60 или 0x62)
    if (!dac.begin(0x60)) {
        Serial.println("Error: MCP4725 not found!");
    }

    // При старте выключаем напряжение на вентиляторе
    dac.setVoltage(0, false);
}



void updateFanLogic() {
    const unsigned long now = millis();

    if (tempHeatsink < 10.0 || tempHeatsink > 100.0) {
        dac.setVoltage(0, false);
        isFanOn = false;
        currentFanVoltage = 0;
        lastFanOutputUpdate = now;
        return;
    }

    if (tempHeatsink >= fanOnTemp) {
        const int startLevel = constrain(fanStartLevel, 0, 100);
        const int minDac = (startLevel * 4095) / 100;

        if (!isFanOn) {
            // Начинаем с заданного минимального уровня вместо импульса на 100%.
            currentFanVoltage = minDac;
            lastFanOutputUpdate = now;
            dac.setVoltage((uint16_t)currentFanVoltage, false);
            isFanOn = true;
        }

        float tempRange = maxFanTemp - fanOnTemp;
        if (tempRange < 0.5) tempRange = 0.5;

        float progress = (tempHeatsink - fanOnTemp) / tempRange;
        progress = constrain(progress, 0.0, 1.0);
        const float targetSpeed = minDac + (progress * progress * (4095 - minDac));

        const unsigned long elapsed = now - lastFanOutputUpdate;
        lastFanOutputUpdate = now;
        const float rampUpPerMs = 4095.0f * fanRampUpPercentPerSecond / 100.0f / 1000.0f;
        const float rampDownPerMs = 4095.0f * fanRampDownPercentPerSecond / 100.0f / 1000.0f;
        const float maxChange = (targetSpeed > currentFanVoltage ? rampUpPerMs : rampDownPerMs) * elapsed;

        if (targetSpeed > currentFanVoltage) {
            currentFanVoltage = min(targetSpeed, currentFanVoltage + maxChange);
        } else {
            currentFanVoltage = max(targetSpeed, currentFanVoltage - maxChange);
        }

        dac.setVoltage((uint16_t)currentFanVoltage, false);

    } else if (tempHeatsink < fanOnTemp - fanHysteresis) {
        dac.setVoltage(0, false);
        isFanOn = false;
        currentFanVoltage = 0;
        lastFanOutputUpdate = now;
    }
}
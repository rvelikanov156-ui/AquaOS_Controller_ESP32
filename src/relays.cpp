#include "relays.h"
#include <Adafruit_PWMServoDriver.h>

//extern Adafruit_PWMServoDriver pwm; 

// Массив пинов для удобного обращения к ним в цикле
const int relayPins[4] = {RELAY1_PIN, RELAY2_PIN, RELAY3_PIN, RELAY4_PIN};

// Текстовые метки по умолчанию
String relayNames[4] = {"Relay 1", "Relay 2", "Relay 3", "Relay 4"};

bool relayState[4] = {false, false, false, false}; //[cite: 1]
int controlType[4] = {0, 0, 0, 0}; //[cite: 1]
float setPoint[4] = {25.0, 25.0, 25.0, 25.0}; //[cite: 1]
float hysteresis[4] = {2.0, 2.0, 2.0, 2.0}; //[cite: 1]
bool schedule[4][48] = {0}; //[cite: 1]
static bool maintenanceOff[4] = {false, false, false, false};
static unsigned long maintenanceStartedAt[4] = {0, 0, 0, 0};
static const unsigned long maintenanceDurationMs = 30UL * 60UL * 1000UL;
static bool feedingPauseOff[4] = {false, false, false, false};
static unsigned long feedingPauseStartedAt[4] = {0, 0, 0, 0};
static const unsigned long feedingPauseDurationMs = 10UL * 60UL * 1000UL;

static bool isFilterRelay(int channel) {
    String lowerName = relayNames[channel];
    lowerName.toLowerCase();
    return lowerName.indexOf("filter") >= 0
        || relayNames[channel].indexOf("Фильтр") >= 0
        || relayNames[channel].indexOf("фильтр") >= 0;
}

static void applyRelayState(int channel, bool state) {
    relayState[channel] = state;
    digitalWrite(relayPins[channel], state ? LOW : HIGH);
}

void initRelays() {
    for (int i = 0; i < 4; i++) {
        pinMode(relayPins[i], OUTPUT);
        // По умолчанию выключаем реле
        digitalWrite(relayPins[i], HIGH); 
    }
}

void updateRelays(float currentTempWater, int currentHour, int currentMinute) {
    for (int i = 0; i < 4; i++) {
        if (maintenanceOff[i]) {
            if (millis() - maintenanceStartedAt[i] < maintenanceDurationMs) {
                if (relayState[i]) applyRelayState(i, false);
                continue;
            }

            maintenanceOff[i] = false;
            Serial.printf("Filter relay %d (%s) returned to automatic control after maintenance\n",
                          i + 1, relayNames[i].c_str());
        }

        if (feedingPauseOff[i]) {
            if (millis() - feedingPauseStartedAt[i] < feedingPauseDurationMs) {
                if (relayState[i]) applyRelayState(i, false);
                continue;
            }

            feedingPauseOff[i] = false;
            Serial.printf("Filter relay %d (%s) returned to automatic control after feeding\n",
                          i + 1, relayNames[i].c_str());
        }

        bool newRelayState = relayState[i]; 

        // Режим 0: Термостат
        if (controlType[i] == 0) { 
            if (currentTempWater >= setPoint[i] + hysteresis[i]) { 
                newRelayState = false; // Достигли порога, выключаем
            } else if (currentTempWater <= setPoint[i] - hysteresis[i]) { 
                newRelayState = true;  // Остыло, включаем
            }
        } 
        // Режим 1: Расписание
        else if (controlType[i] == 1) { 
            int currentInterval = (currentHour * 2) + (currentMinute / 30); 
            newRelayState = schedule[i][currentInterval]; 
        }
        
        // Применяем новое состояние, если оно изменилось
        if (newRelayState != relayState[i]) { 
            applyRelayState(i, newRelayState);
        }
    }
}

bool toggleFilterMaintenance() {
    for (int i = 0; i < 4; i++) {
        if (!isFilterRelay(i)) continue;

        maintenanceOff[i] = !maintenanceOff[i];
        if (maintenanceOff[i]) {
            maintenanceStartedAt[i] = millis();
            applyRelayState(i, false);
            Serial.printf("Filter relay %d (%s) disabled for maintenance\n",
                          i + 1, relayNames[i].c_str());
        } else {
            Serial.printf("Filter relay %d (%s) returned to automatic control\n",
                          i + 1, relayNames[i].c_str());
        }
        return true;
    }

    Serial.println("Filter relay not found. Name its outlet 'Filter' or 'Фильтр'.");
    return false;
}

void pauseFilterForFeeding() {
    for (int i = 0; i < 4; i++) {
        if (!isFilterRelay(i)) continue;

        feedingPauseOff[i] = true;
        feedingPauseStartedAt[i] = millis();
        applyRelayState(i, false);
        Serial.printf("Filter relay %d (%s) disabled for feeding\n",
                      i + 1, relayNames[i].c_str());
        return;
    }

    Serial.println("Filter relay not found. Name its outlet 'Filter' or 'Фильтр'.");
}
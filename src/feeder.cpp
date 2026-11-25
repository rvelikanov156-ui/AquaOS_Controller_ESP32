#include "feeder.h"
#include "relays.h"

// Значения по умолчанию
uint8_t feed1Hour = 8;        //
uint8_t feed1Minute = 0;      //
uint8_t feed2Hour = 18;       //
uint8_t feed2Minute = 0;      //
uint32_t feedDuration = 5000; //
bool feedFilterPauseEnabled = true;

bool isFeeding = false;       //
unsigned long feedStartTime = 0; //

// Не допускаем повторный запуск до выхода из минуты кормления по расписанию.
static bool scheduleMinuteHandled = false;

void initFeeder() {
    pinMode(FEEDER_PIN, OUTPUT); //
    digitalWrite(FEEDER_PIN, LOW); // Выключено по умолчанию
}

// Функция для принудительного запуска кормушки (например, кнопкой)
void manualFeed() {
    if (!isFeeding) {
        isFeeding = true;
        feedStartTime = millis();
        digitalWrite(FEEDER_PIN, HIGH);
    }
}

void updateFeeder(int currentHour, int currentMinute) {
    unsigned long currentMillis = millis();

    bool feed1Active = (feed1Hour != 25 && currentHour == feed1Hour && currentMinute == feed1Minute);
    bool feed2Active = (feed2Hour != 25 && currentHour == feed2Hour && currentMinute == feed2Minute);
    bool scheduledFeedActive = feed1Active || feed2Active;

    if (scheduledFeedActive && !scheduleMinuteHandled) {
        scheduleMinuteHandled = true;
        if (feedFilterPauseEnabled) {
            pauseFilterForFeeding();
        }
        if (!isFeeding) {
            isFeeding = true;
            feedStartTime = currentMillis;
            digitalWrite(FEEDER_PIN, HIGH);
        }
    } else if (!scheduledFeedActive) {
        scheduleMinuteHandled = false;
    }

    // Отключение кормушки после истечения времени duration
    if (isFeeding && (currentMillis - feedStartTime >= feedDuration)) { //
        isFeeding = false; //
        digitalWrite(FEEDER_PIN, LOW); // Выключаем транзистор
    }
}
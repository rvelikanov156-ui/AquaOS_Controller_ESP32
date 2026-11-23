#include "temperature.h"

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Массивы для хранения уникальных адресов двух датчиков
DeviceAddress addressWater;
DeviceAddress addressHeatsink;

float tempWater = 25.0;
float tempHeatsink = 25.0;

unsigned long lastTempRequestTime = 0;
const unsigned long tempRequestInterval = 5000; // Интервал запроса 5 секунд
bool requestPending = false;

void initTemperatureSensors() {
    sensors.begin();
    
    // ВАЖНО: Отключаем ожидание конвертации, чтобы не блокировать цикл ESP32
    sensors.setWaitForConversion(false); 

    // Поиск адресов датчиков на шине по индексу (0 и 1)
    if (sensors.getAddress(addressWater, 0)) {
        sensors.setResolution(addressWater, 12); // Устанавливаем 12 бит
    } else {
        Serial.println("Error: Water temp sensor not found!");
    }
    
    if (sensors.getAddress(addressHeatsink, 1)) {
        sensors.setResolution(addressHeatsink, 12);
    } else {
        Serial.println("Error: Heatsink temp sensor not found!");
    }
}

void updateTemperatures() {
    unsigned long currentMillis = millis();

    // Шаг 1: Отправляем команду на старт измерения (раз в 5 секунд)
    if (!requestPending && (currentMillis - lastTempRequestTime >= tempRequestInterval)) {
        sensors.requestTemperatures(); 
        lastTempRequestTime = currentMillis;
        requestPending = true;
    }

    // Шаг 2: Считываем результат спустя 800 мс (время конвертации 12-битного датчика ~750 мс)
    if (requestPending && (currentMillis - lastTempRequestTime >= 800)) {
        float tWater = sensors.getTempC(addressWater);
        float tHeatsink = sensors.getTempC(addressHeatsink);

        // Проверка на ошибки чтения (-127.0 C) и отсеивание ложных скачков
        if (tWater != DEVICE_DISCONNECTED_C && tWater > -10.0 && tWater < 80.0) {
            tempWater = tWater;
        }
        
        if (tHeatsink != DEVICE_DISCONNECTED_C && tHeatsink > -10.0 && tHeatsink < 100.0) {
            tempHeatsink = tHeatsink;
        }

        // Завершили цикл чтения, ждем следующего интервала
        requestPending = false; 
    }
}
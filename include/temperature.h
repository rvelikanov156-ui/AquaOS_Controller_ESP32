#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Пин шины 1-Wire на ESP32 (задайте нужный GPIO)
#define ONE_WIRE_BUS 4 

// Глобальные переменные температур, доступные другим модулям
extern float tempWater;    
extern float tempHeatsink; 

void initTemperatureSensors();
void updateTemperatures();

#endif // TEMPERATURE_H
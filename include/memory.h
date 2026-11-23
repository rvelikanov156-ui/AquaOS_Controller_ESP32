#ifndef MEMORY_H
#define MEMORY_H

#include <Arduino.h>
#include <Preferences.h>

// Экземпляр класса для работы с памятью
extern Preferences preferences;
extern String ssid;
extern String password;
extern long gmtOffset_sec;
extern bool mqttEnabled;
extern String mqttCustomDeviceId;
extern String mqttHost;
extern uint16_t mqttPort;
extern String mqttUsername;
extern String mqttPassword;
extern String mqttCaCertificate;
extern bool mqttUseTls;
extern int daylightOffset_sec;
// Главные функции
void initMemory();       // Вызывается один раз при старте
void loadAllSettings();  // Чтение из памяти
void saveAllSettings();  // Запись в память (вызывать при выходе из меню)

void saveSystemSettings();
void saveFanSettings();
void saveLightBrightness();
void saveMoonBrightness();
void saveLightTimers();
void saveRelaySettings();
void saveRelaySchedule();
void saveDoserSettings();
void saveDoserSchedule();
void saveFeederSettings();
void saveMqttSettings();

#endif // MEMORY_H
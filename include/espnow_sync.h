#ifndef ESPNOW_SYNC_H
#define ESPNOW_SYNC_H

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// Структура данных от Master (ESP32) к Slave (ESP8266 в крышке)
typedef struct __attribute__((packed)) struct_message_to_slave {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    float tempWater;
    float tempHeatsink;
    bool isFanOn;
    bool isMoonlight;
    bool isDayPause;
    bool isLightOn;
    uint8_t brightness[5]; // Яркость 0-100% для 5 каналов (R, B, W1, W2, W3)
    uint8_t wifiChannel;
    uint8_t configuredBrightness[5];
    uint8_t moonBrightness[5];
    uint8_t lightOnHour;
    uint8_t lightOnMinute;
    uint8_t lightOffHour;
    uint8_t lightOffMinute;
    uint8_t dayPauseStartHour;
    uint8_t dayPauseStartMinute;
    uint8_t dayPauseEndHour;
    uint8_t dayPauseEndMinute;
    uint8_t sunriseDuration;
    uint8_t feed1Hour;
    uint8_t feed1Minute;
    uint8_t feed2Hour;
    uint8_t feed2Minute;
    uint16_t feedDurationDeciseconds;
    bool moonlightEnabled;
    bool isFeeding;
    uint8_t remoteMenuMode;
    uint8_t remoteMenuItem;
    uint8_t remoteMenuField;
} struct_message_to_slave;

typedef struct __attribute__((packed)) struct_message_from_slave {
    uint8_t command; // 0 - нажата кнопка, 1 - настройка яркости
    uint8_t param1;  // ID кнопки ИЛИ Номер канала (0-4)
    uint8_t param2;  // Тип нажатия ИЛИ Значение яркости (0-100)
} struct_message_from_slave;

enum OtaPacketType : uint8_t {
    OTA_START = 0xA0,
    OTA_DATA = 0xA1,
    OTA_END = 0xA2,
    OTA_ABORT = 0xA3,
    OTA_ACK = 0xA4
};

typedef struct __attribute__((packed)) MonitorOtaPacket {
    uint8_t type;
    uint16_t seq;
    uint16_t len;
    uint32_t totalSize;
    uint8_t payload[200];
} MonitorOtaPacket;

extern struct_message_to_slave displayData;

void initESPNOW();
void sendDataToDisplay();
void processRemoteCommands();
void setMonitorOtaTransferActive(bool active);
bool sendMonitorOtaStart(uint32_t totalSize);
bool sendMonitorOtaData(uint16_t seq, const uint8_t *data, uint16_t len);
bool sendMonitorOtaEnd();

#endif // ESPNOW_SYNC_H
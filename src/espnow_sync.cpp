#include "espnow_sync.h"
#include "lighting.h"
#include "feeder.h"
#include "memory.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Широковещательный MAC-адрес (отправляем всем) B4:E6:2D:3C:8E:63
uint8_t slaveAddress[] = {0xB4, 0xE6, 0x2D, 0x3C, 0x8E, 0x63};
static const uint8_t otaBroadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct_message_to_slave displayData;
struct_message_from_slave incomingCommand;
static QueueHandle_t remoteCommandQueue = nullptr;

static volatile bool monitorOtaAckReceived = false;
static volatile bool monitorOtaAckSuccess = false;
static volatile uint8_t monitorOtaAckType = 0;
static volatile uint16_t monitorOtaAckSeq = 0;
static volatile bool monitorOtaTransferActive = false;
static uint8_t remoteMenuMode = 0;
static uint8_t remoteMenuItem = 0;
static uint8_t remoteMenuField = 0;
static bool remoteMenuDirty = false;

static uint8_t remoteMenuFieldCount() {
    switch (remoteMenuItem) {
        case 0: return 5;
        case 1: case 2: return 4;
        case 3: return 1;
        case 4: return 6;
        case 6: return 6;
        default: return 0;
    }
}

static int adjustOptionalHour(int hour, int direction) {
    if (direction > 0) {
        if (hour == 25) return 0;
        return hour >= 23 ? 25 : hour + 1;
    }
    if (hour == 25) return 23;
    return hour <= 0 ? 25 : hour - 1;
}

static int adjustMinute(int minute, int direction) {
    return (minute + direction + 60) % 60;
}

static void saveRemoteMenuSettings() {
    if (!remoteMenuDirty) return;
    switch (remoteMenuItem) {
        case 0: saveLightBrightness(); break;
        case 1: case 2: case 3: saveLightTimers(); break;
        case 4: saveFeederSettings(); break;
        case 6:
            saveMoonBrightness();
            saveLightTimers();
            break;
    }
    remoteMenuDirty = false;
}

static void adjustRemoteSetting(int direction) {
    switch (remoteMenuItem) {
        case 0:
            brightnessSettings[remoteMenuField] = constrain(
                brightnessSettings[remoteMenuField] + direction, 0, 100);
            if (isLightOn) setPWMOutputs(brightnessSettings);
            break;
        case 1:
            if (remoteMenuField == 0) dayPauseStartHour = adjustOptionalHour(dayPauseStartHour, direction);
            else if (remoteMenuField == 1) dayPauseStartMinute = adjustMinute(dayPauseStartMinute, direction);
            else if (remoteMenuField == 2) dayPauseEndHour = adjustOptionalHour(dayPauseEndHour, direction);
            else dayPauseEndMinute = adjustMinute(dayPauseEndMinute, direction);
            break;
        case 2:
            if (remoteMenuField == 0) lightOnHour = (lightOnHour + direction + 24) % 24;
            else if (remoteMenuField == 1) lightOnMinute = adjustMinute(lightOnMinute, direction);
            else if (remoteMenuField == 2) lightOffHour = (lightOffHour + direction + 24) % 24;
            else lightOffMinute = adjustMinute(lightOffMinute, direction);
            break;
        case 3:
            sunriseDuration = constrain(sunriseDuration + direction * 5, 0, 240);
            break;
        case 4:
            if (remoteMenuField == 0) feed1Hour = adjustOptionalHour(feed1Hour, direction);
            else if (remoteMenuField == 1) feed1Minute = adjustMinute(feed1Minute, direction);
            else if (remoteMenuField == 2) feed2Hour = adjustOptionalHour(feed2Hour, direction);
            else if (remoteMenuField == 3) feed2Minute = adjustMinute(feed2Minute, direction);
            else if (remoteMenuField == 4 && direction > 0 && feedDuration <= 599900) feedDuration += 100;
            else if (remoteMenuField == 4 && direction < 0 && feedDuration > 100) feedDuration -= 100;
            else if (remoteMenuField == 5) feedFilterPauseEnabled = direction > 0;
            break;
        case 6:
            if (remoteMenuField < 5) {
                moonlightSettings[remoteMenuField] = constrain(
                    moonlightSettings[remoteMenuField] + direction, 0, 100);
                if (!isLightOn) setPWMOutputs(moonlightSettings);
            } else {
                isMoonlightEnabled = direction > 0;
                if (!isLightOn) {
                    if (isMoonlightEnabled) setPWMOutputs(moonlightSettings);
                    else turnOffAllChannels();
                }
            }
            break;
    }
    remoteMenuDirty = true;
}

static void processRemoteButton(uint8_t buttonId, uint8_t eventType) {
    if (eventType != 0 || buttonId < 1 || buttonId > 4) return;

    if (remoteMenuMode == 0) {
        if (buttonId == 4) {
            remoteMenuMode = 1;
            remoteMenuItem = 0;
        }
        return;
    }

    if (remoteMenuMode == 1) {
        if (buttonId == 1) remoteMenuItem = (remoteMenuItem + 6) % 7;
        else if (buttonId == 2) remoteMenuItem = (remoteMenuItem + 1) % 7;
        else if (buttonId == 3) {
            remoteMenuMode = 0;
        } else if (buttonId == 4) {
            if (remoteMenuItem == 5) {
                manualFeed();
            } else {
                remoteMenuMode = 2;
                remoteMenuField = 0;
                remoteMenuDirty = false;
            }
        }
        return;
    }

    if (buttonId == 3) {
        saveRemoteMenuSettings();
        remoteMenuMode = 1;
    } else if (buttonId == 4) {
        uint8_t fieldCount = remoteMenuFieldCount();
        if (fieldCount > 0) remoteMenuField = (remoteMenuField + 1) % fieldCount;
    } else if (buttonId == 1) {
        adjustRemoteSetting(1);
    } else if (buttonId == 2) {
        adjustRemoteSetting(-1);
    }
}

void setMonitorOtaTransferActive(bool active) {
    monitorOtaTransferActive = active;
}

static bool sendMonitorOtaPacketAndWait(const MonitorOtaPacket &pkt) {
    const uint8_t maxAttempts = 8;
    const uint32_t ackTimeoutMs = 2000;

    for (uint8_t attempt = 0; attempt < maxAttempts; attempt++) {
        monitorOtaAckReceived = false;
        if (pkt.type == OTA_START) {
            Serial.printf("OTA_START radio channel now=%u mode=%d\n",
                          WiFi.channel(), (int)WiFi.getMode());
        }
        esp_err_t result = esp_now_send(otaBroadcastAddress, (const uint8_t *)&pkt, sizeof(pkt));
        Serial.printf("OTA TX type=%u seq=%u attempt=%u result=%d\n",
                      pkt.type, pkt.seq, attempt + 1, result);
        if (result != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        uint32_t startedAt = millis();
        while (millis() - startedAt < ackTimeoutMs) {
            if (monitorOtaAckReceived && monitorOtaAckType == pkt.type && monitorOtaAckSeq == pkt.seq) {
                Serial.printf("OTA ACK type=%u seq=%u success=%u\n",
                              monitorOtaAckType, monitorOtaAckSeq, monitorOtaAckSuccess ? 1 : 0);
                return monitorOtaAckSuccess;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        Serial.printf("OTA ACK timeout type=%u seq=%u\n", pkt.type, pkt.seq);
    }

    return false;
}

bool sendMonitorOtaStart(uint32_t totalSize) {
    MonitorOtaPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = OTA_START;
    pkt.seq = 0;
    pkt.len = 0;
    pkt.totalSize = totalSize;
    return sendMonitorOtaPacketAndWait(pkt);
}

bool sendMonitorOtaData(uint16_t seq, const uint8_t *data, uint16_t len) {
    MonitorOtaPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = OTA_DATA;
    pkt.seq = seq;
    pkt.len = len;
    pkt.totalSize = 0;
    memcpy(pkt.payload, data, len);
    return sendMonitorOtaPacketAndWait(pkt);
}

bool sendMonitorOtaEnd() {
    MonitorOtaPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = OTA_END;
    pkt.seq = 0;
    pkt.len = 0;
    return sendMonitorOtaPacketAndWait(pkt);
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
    if (len == sizeof(MonitorOtaPacket)) {
        const MonitorOtaPacket *pkt = (const MonitorOtaPacket *)incomingData;
        if (pkt->type == OTA_ACK) {
            monitorOtaAckType = (uint8_t)pkt->len;
            monitorOtaAckSeq = pkt->seq;
            monitorOtaAckSuccess = pkt->totalSize == 0;
            monitorOtaAckReceived = true;
            return;
        }
        return;
    }

    if (len == sizeof(struct_message_from_slave)) {
        memcpy(&incomingCommand, incomingData, sizeof(incomingCommand));
        Serial.printf("Monitor command RX: command=%u button=%u event=%u\n",
                      incomingCommand.command, incomingCommand.param1, incomingCommand.param2);
        if ((incomingCommand.command == 0 || incomingCommand.command == 1) &&
            (remoteCommandQueue == nullptr || xQueueSend(remoteCommandQueue, &incomingCommand, 0) != pdTRUE)) {
            Serial.println("Monitor command queue full or unavailable");
        }
        return;
    }
}

void initESPNOW() {
    remoteCommandQueue = xQueueCreate(8, sizeof(struct_message_from_slave));
    if (remoteCommandQueue == nullptr) {
        Serial.println("Failed to create monitor command queue");
        return;
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }

    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, slaveAddress, 6);

    if (WiFi.getMode() == WIFI_AP) {
        peerInfo.ifidx = WIFI_IF_AP;
        Serial.println("ESP-NOW Interface: AP");
    } else {
        peerInfo.ifidx = WIFI_IF_STA;
        Serial.println("ESP-NOW Interface: STA");
    }

    peerInfo.channel = 0;
    Serial.print("ESP-NOW current channel: ");
    Serial.println(WiFi.channel());

    peerInfo.encrypt = false;

    if (esp_now_is_peer_exist(slaveAddress)) {
        esp_now_del_peer(slaveAddress);
    }

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add peer (Display)");
        return;
    }

    memcpy(peerInfo.peer_addr, otaBroadcastAddress, sizeof(peerInfo.peer_addr));
    if (!esp_now_is_peer_exist(otaBroadcastAddress) && esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add OTA broadcast peer");
        return;
    }

    Serial.println("ESP-NOW Init Success!");
}

void processRemoteCommands() {
    if (remoteCommandQueue == nullptr) return;

    struct_message_from_slave command;
    while (xQueueReceive(remoteCommandQueue, &command, 0) == pdTRUE) {
        if (command.command == 0) {
            processRemoteButton(command.param1, command.param2);
        } else if (command.command == 1) {
            int channel = command.param1;
            int value = command.param2;
            if (channel >= 0 && channel < 5) {
                brightnessSettings[channel] = value;
                if (displayData.isLightOn) setPWMOutputs(brightnessSettings);
            }
        }

        sendDataToDisplay();
    }
}

void sendDataToDisplay() {
    if (monitorOtaTransferActive) return;

    for (uint8_t channel = 0; channel < 5; channel++) {
        displayData.configuredBrightness[channel] = constrain(brightnessSettings[channel], 0, 100);
        displayData.moonBrightness[channel] = constrain(moonlightSettings[channel], 0, 100);
    }
    displayData.lightOnHour = lightOnHour;
    displayData.lightOnMinute = lightOnMinute;
    displayData.lightOffHour = lightOffHour;
    displayData.lightOffMinute = lightOffMinute;
    displayData.dayPauseStartHour = dayPauseStartHour;
    displayData.dayPauseStartMinute = dayPauseStartMinute;
    displayData.dayPauseEndHour = dayPauseEndHour;
    displayData.dayPauseEndMinute = dayPauseEndMinute;
    displayData.sunriseDuration = constrain(sunriseDuration, 0, 255);
    displayData.feed1Hour = feed1Hour;
    displayData.feed1Minute = feed1Minute;
    displayData.feed2Hour = feed2Hour;
    displayData.feed2Minute = feed2Minute;
    uint32_t feedDurationDeciseconds = feedDuration / 100;
    displayData.feedDurationDeciseconds = feedDurationDeciseconds > 65535
        ? 65535 : feedDurationDeciseconds;
    displayData.moonlightEnabled = isMoonlightEnabled;
    displayData.isFeeding = isFeeding;
    displayData.remoteMenuMode = remoteMenuMode;
    displayData.remoteMenuItem = remoteMenuItem;
    displayData.remoteMenuField = remoteMenuField;

    esp_err_t result = esp_now_send(slaveAddress, (uint8_t *)&displayData, sizeof(displayData));
    if (result != ESP_OK) {
        // Ошибка отправки пакета
    }
}
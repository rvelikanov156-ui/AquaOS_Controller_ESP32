#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include <Arduino.h>

void mqttServiceLoop();
String mqttDeviceId();
bool isMqttServiceConnected();
String mqttServiceStatus();

#endif

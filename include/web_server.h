#ifndef WEB_SERVER_H
#define WEB_SERVER_H

//#include <Arduino.h>
#include <ArduinoJson.h>

void initWebServer();
void handleClientWebServer();
bool dispatchMqttApiRequest(
    const String& method,
    const String& path,
    JsonObjectConst args,
    int& statusCode,
    String& contentType,
    String& responseBody
);
void loadAllSettings();
void saveAllSettings();

#endif // WEB_SERVER_H
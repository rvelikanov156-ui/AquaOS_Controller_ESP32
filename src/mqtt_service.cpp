#include "mqtt_service.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "memory.h"
#include "web_server.h"

namespace {
WiFiClient plainClient;
WiFiClientSecure secureClient;
PubSubClient mqttClient(plainClient);
String connectedConfiguration;
unsigned long lastConnectAttempt = 0;
String availabilityTopic;
String requestTopic;
bool hasAttemptedConnection = false;

String currentConfiguration() {
    return String(mqttEnabled) + "|" + mqttCustomDeviceId + "|" + mqttHost + "|" +
        String(mqttPort) + "|" +
        mqttUsername + "|" + mqttPassword + "|" + String(mqttUseTls) + "|" +
        mqttCaCertificate;
}

void handleMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
    if (String(topic) != requestTopic) return;
    JsonDocument request;
    const DeserializationError parseError = deserializeJson(request, payload, length);
    if (parseError) {
        Serial.printf("MQTT request JSON error: %s\n", parseError.c_str());
        return;
    }

    const char* requestId = request["requestId"] | "";
    const char* responseTopic = request["responseTopic"] | "";
    const char* method = request["method"] | "";
    const char* path = request["path"] | "";
    JsonObjectConst args = request["args"].as<JsonObjectConst>();
    if (!requestId[0] || !responseTopic[0] || !method[0] || !path[0]) {
        Serial.println("Rejected incomplete MQTT request");
        return;
    }
    const String expectedResponsePrefix = "aquaos/" + mqttDeviceId() + "/response/";
    const String responseTopicValue(responseTopic);
    if (!responseTopicValue.startsWith(expectedResponsePrefix) ||
        responseTopicValue.length() > 128 ||
        responseTopicValue.indexOf('+') >= 0 ||
        responseTopicValue.indexOf('#') >= 0) {
        Serial.println("Rejected MQTT response topic outside this device namespace");
        return;
    }

    int statusCode = 500;
    String contentType = "text/plain";
    String responseBody;
    const bool dispatched = dispatchMqttApiRequest(
        method, path, args, statusCode, contentType, responseBody
    );
    JsonDocument response;
    response["requestId"] = requestId;
    response["status"] = dispatched ? statusCode : 404;
    response["contentType"] = contentType;
    response["body"] = dispatched ? responseBody : "Unsupported API request";
    String serialized;
    serializeJson(response, serialized);
    if (!mqttClient.publish(responseTopic, serialized.c_str(), false)) {
        Serial.println("Failed to publish MQTT API response");
    }
    if (String(path) == "/api/set_sys" && String(method) == "POST") {
        delay(500);
        ESP.restart();
    }
}

bool connectToBroker() {
    if (!mqttEnabled || mqttHost.isEmpty() || WiFi.status() != WL_CONNECTED) return false;
    if (mqttUseTls) {
        if (mqttCaCertificate.isEmpty()) {
            Serial.println("MQTT TLS is enabled but no CA certificate is configured");
            return false;
        }
        secureClient.setCACert(mqttCaCertificate.c_str());
        mqttClient.setClient(secureClient);
    } else {
        mqttClient.setClient(plainClient);
    }
    mqttClient.setServer(mqttHost.c_str(), mqttPort);
    if (!mqttClient.setBufferSize(4096)) {
        Serial.println("Unable to allocate MQTT packet buffer");
        return false;
    }
    mqttClient.setCallback(handleMqttMessage);

    const String clientId = "aquaos-" + mqttDeviceId();
    hasAttemptedConnection = true;
    Serial.printf(
        "Starting MQTT connection attempt on port %u (%s)\n",
        mqttPort,
        mqttUseTls ? "TLS" : "plain"
    );
    const bool hasCredentials = !mqttUsername.isEmpty() || !mqttPassword.isEmpty();
    const bool connected = hasCredentials
        ? mqttClient.connect(
            clientId.c_str(),
            mqttUsername.c_str(),
            mqttPassword.c_str(),
            availabilityTopic.c_str(),
            1,
            true,
            "offline"
        )
        : mqttClient.connect(
            clientId.c_str(),
            nullptr,
            nullptr,
            availabilityTopic.c_str(),
            1,
            true,
            "offline"
        );
    if (!connected) {
        Serial.printf("MQTT connection failed, state=%d\n", mqttClient.state());
        return false;
    }

    mqttClient.publish(availabilityTopic.c_str(), "online", true);
    if (!mqttClient.subscribe(requestTopic.c_str(), 1)) {
        Serial.println("MQTT request subscription failed");
        mqttClient.disconnect();
        return false;
    }
    connectedConfiguration = currentConfiguration();
    Serial.printf("MQTT connected for device %s\n", mqttDeviceId().c_str());
    return true;
}
}

String mqttDeviceId() {
    if (!mqttCustomDeviceId.isEmpty()) return mqttCustomDeviceId;
    String id = WiFi.macAddress();
    id.replace(":", "");
    id.toLowerCase();
    return id;
}

bool isMqttServiceConnected() {
    return mqttClient.connected();
}

String mqttServiceStatus() {
    if (!mqttEnabled) return "MQTT is disabled";
    if (WiFi.status() != WL_CONNECTED) return "Wi-Fi is disconnected";
    if (mqttHost.isEmpty()) return "Broker host is empty";
    if (mqttUseTls && mqttCaCertificate.isEmpty()) return "TLS CA certificate is missing";
    if (mqttClient.connected()) return "Connected";
    if (!hasAttemptedConnection) return "Waiting for first connection attempt";

    const int state = mqttClient.state();
    switch (state) {
        case 1: return "Broker rejected MQTT protocol version";
        case 2: return "Broker rejected client ID";
        case 3: return "Broker is unavailable";
        case 4: return "Broker rejected credentials";
        case 5: return "Broker denied authentication or permissions";
        case -2:
            return mqttUseTls
                ? "TLS/network failure (state -2): check CA, clock, host and port"
                : "Network connection failed (state -2)";
        case -3: return "MQTT connection was lost (state -3)";
        case -4: return "MQTT connection timed out (state -4)";
        default: return "MQTT disconnected (state " + String(state) + ")";
    }
}

void mqttServiceLoop() {
    const String configuration = currentConfiguration();
    if (configuration != connectedConfiguration) {
        if (mqttClient.connected()) mqttClient.disconnect();
        connectedConfiguration = "";
    }
    if (!mqttEnabled || WiFi.status() != WL_CONNECTED) {
        if (mqttClient.connected()) mqttClient.disconnect();
        return;
    }

    const String deviceId = mqttDeviceId();
    availabilityTopic = "aquaos/" + deviceId + "/availability";
    requestTopic = "aquaos/" + deviceId + "/request";
    if (!mqttClient.connected()) {
        if (millis() - lastConnectAttempt >= 10000UL) {
            lastConnectAttempt = millis();
            connectToBroker();
        }
        return;
    }
    mqttClient.loop();
}

#include "memory.h"

// Подключаем модули, чьи настройки нужно сохранять
#include "fan.h"
#include "relays.h"
#include "lighting.h"
#include "doser.h"
#include "feeder.h"

Preferences preferences;

// Глобальные системные переменные
String ssid = "ESP_Aqua";
String password = "";
long gmtOffset_sec = 3600;
int daylightOffset_sec = 0;
bool mqttEnabled = false;
String mqttCustomDeviceId = "";
String mqttHost = "";
uint16_t mqttPort = 8883;
String mqttUsername = "";
String mqttPassword = "";
String mqttCaCertificate = "";
bool mqttUseTls = true;

void initMemory() {
    // Открываем хранилище. false означает режим чтение/запись
    preferences.begin("aqua_cfg", false);

    // Сразу при загрузке ESP32 вытягиваем все настройки
    loadAllSettings();
}

void loadAllSettings() {
    // --- 1. СИСТЕМА ---
    ssid = preferences.getString("ssid", "ESP_Aqua");
    password = preferences.getString("pass", "12345678");
    gmtOffset_sec = preferences.getLong("gmt", 3600);
    mqttEnabled = preferences.getBool("mqttEn", false);
    mqttCustomDeviceId = preferences.getString("mqttId", "");
    mqttHost = preferences.getString("mqttHost", "");
    mqttPort = preferences.getUShort("mqttPort", 8883);
    mqttUsername = preferences.getString("mqttUser", "");
    mqttPassword = preferences.getString("mqttPass", "");
    mqttCaCertificate = preferences.getString("mqttCa", "");
    mqttUseTls = preferences.getBool("mqttTls", true);

    // --- 2. ВЕНТИЛЯТОР ---
    fanOnTemp = preferences.getFloat("fanOnTemp", 30.0);
    maxFanTemp = preferences.getFloat("maxFanTemp", 40.0);
    fanHysteresis = preferences.getFloat("fanHyst", 5.0);
    fanStartLevel = preferences.getInt("fanStart", 30);

    // --- 3. ИМЕНА РЕЛЕ И ДОЗАТОРОВ ---
    for (int i = 0; i < 4; i++) {
        // Генерируем ключи rNm0, rNm1... и dNm0, dNm1...
        String rKey = "rNm" + String(i);
        relayNames[i] = preferences.getString(rKey.c_str(), "Relay " + String(i + 1));

        String dKey = "dNm" + String(i);
        doserNames[i] = preferences.getString(dKey.c_str(), "Doser " + String(i + 1));
    }

    // --- 4. РЕЛЕ (Массивы) ---
    if (preferences.getBytesLength("relCtrl") > 0) {
        preferences.getBytes("relCtrl", controlType, sizeof(controlType));
        preferences.getBytes("relSetP", setPoint, sizeof(setPoint));
        preferences.getBytes("relHyst", hysteresis, sizeof(hysteresis));
        preferences.getBytes("relSched", schedule, sizeof(schedule)); // Читаем все 192 чекбокса
    }

    // --- 5. ДОЗАТОРЫ (Массивы) ---
    if (preferences.getBytesLength("dRunT") == sizeof(fertMotorRunTime)) {
        preferences.getBytes("dRunT", fertMotorRunTime, sizeof(fertMotorRunTime));
    }
    if (preferences.getBytesLength("dTrgH") == sizeof(fertTriggerHour)) {
        preferences.getBytes("dTrgH", fertTriggerHour, sizeof(fertTriggerHour));
    }
    if (preferences.getBytesLength("dTrgM") == sizeof(fertTriggerMinute)) {
        preferences.getBytes("dTrgM", fertTriggerMinute, sizeof(fertTriggerMinute));
    }
    if (preferences.getBytesLength("dDays") == sizeof(fertDays)) {
        preferences.getBytes("dDays", fertDays, sizeof(fertDays));
    }
    for (int i = 0; i < 4; i++) {
        if (fertMotorRunTime[i] < 1) fertMotorRunTime[i] = 1;
        if (fertMotorRunTime[i] > 300) fertMotorRunTime[i] = 300;
    }
    if (preferences.getBytesLength("dEn") >= sizeof(fertEnabled)) {
        preferences.getBytes("dEn", fertEnabled, sizeof(fertEnabled));
    }

    // --- 6. КОРМУШКА ---
    feed1Hour = preferences.getUChar("f1H", 8);
    feed1Minute = preferences.getUChar("f1M", 0);
    feed2Hour = preferences.getUChar("f2H", 18);
    feed2Minute = preferences.getUChar("f2M", 0);
    feedDuration = preferences.getUInt("fDur", 5000);

    // --- 7. ОСВЕЩЕНИЕ (Расширенное) ---
    if (preferences.getBytesLength("lBrt") > 0) {
        preferences.getBytes("lBrt", brightnessSettings, sizeof(brightnessSettings));
    }
    if (preferences.getBytesLength("mBrt") > 0) {
        preferences.getBytes("mBrt", moonlightSettings, sizeof(moonlightSettings));
    }
    
    // Загрузка таймеров (если памяти еще нет, берем дефолтные значения)
    lightOnHour = preferences.getInt("lOnH", 8);
    lightOnMinute = preferences.getInt("lOnM", 0);
    lightOffHour = preferences.getInt("lOffH", 20);
    lightOffMinute = preferences.getInt("lOffM", 0);
    sunriseDuration = preferences.getInt("sRise", 30);
    
    // 25 - означает, что дневная пауза выключена по умолчанию
    dayPauseStartHour = preferences.getInt("dpStH", 25);
    dayPauseStartMinute = preferences.getInt("dpStM", 0);
    dayPauseEndHour = preferences.getInt("dpEnH", 25);
    dayPauseEndMinute = preferences.getInt("dpEnM", 0);
    
    isMoonlightEnabled = preferences.getBool("moonEn", true);

    Serial.println("Settings successfully loaded from NVS!");
}

// ============================================================================
// ТОЧЕЧНОЕ СОХРАНЕНИЕ ПО МОДУЛЯМ
//
// ВАЖНО: раньше любое изменение одного значения (например, один слайдер
// яркости на веб-странице) вызывало saveAllSettings(), которая писала во
// флеш СРАЗУ ВСЕ ~20 ключей NVS, включая 192-байтный массив расписания реле.
// Запись в NVS — синхронная операция и блокирует loop() на десятки-сотни мс.
// Так как web-сервер, опрос джойстика и вся периферия крутятся в одном
// loop(), это вызывало одновременное "зависание" кнопки и лаги веб-интерфейса
// при каждом изменении настройки.
//
// Теперь каждый модуль сохраняет только СВОИ ключи, поэтому один слайдер
// пишет 1 ключ, а не 20.
// ============================================================================

void saveSystemSettings() {
    preferences.putString("ssid", ssid);
    preferences.putString("pass", password);
    preferences.putLong("gmt", gmtOffset_sec);
    Serial.println("System settings saved!");
}

void saveMqttSettings() {
    preferences.putBool("mqttEn", mqttEnabled);
    preferences.putString("mqttId", mqttCustomDeviceId);
    preferences.putString("mqttHost", mqttHost);
    preferences.putUShort("mqttPort", mqttPort);
    preferences.putString("mqttUser", mqttUsername);
    preferences.putString("mqttPass", mqttPassword);
    preferences.putString("mqttCa", mqttCaCertificate);
    preferences.putBool("mqttTls", mqttUseTls);
    Serial.println("MQTT settings saved!");
}

void saveFanSettings() {
    preferences.putFloat("fanOnTemp", fanOnTemp);
    preferences.putFloat("maxFanTemp", maxFanTemp);
    preferences.putFloat("fanHyst", fanHysteresis);
    preferences.putInt("fanStart", fanStartLevel);
    Serial.println("Fan settings saved!");
}

// Только текущая яркость дневного света (самое частое изменение - слайдеры)
void saveLightBrightness() {
    preferences.putBytes("lBrt", brightnessSettings, sizeof(brightnessSettings));
    Serial.println("Light brightness saved!");
}

// Только яркость ночной (лунной) подсветки
void saveMoonBrightness() {
    preferences.putBytes("mBrt", moonlightSettings, sizeof(moonlightSettings));
    Serial.println("Moon brightness saved!");
}

// Таймеры рассвета/заката и дневной паузы
void saveLightTimers() {
    preferences.putInt("lOnH", lightOnHour);
    preferences.putInt("lOnM", lightOnMinute);
    preferences.putInt("lOffH", lightOffHour);
    preferences.putInt("lOffM", lightOffMinute);
    preferences.putInt("sRise", sunriseDuration);

    preferences.putInt("dpStH", dayPauseStartHour);
    preferences.putInt("dpStM", dayPauseStartMinute);
    preferences.putInt("dpEnH", dayPauseEndHour);
    preferences.putInt("dpEnM", dayPauseEndMinute);

    preferences.putBool("moonEn", isMoonlightEnabled);
    Serial.println("Light timers saved!");
}

// Имена реле + режим/уставки термостата (без 192-байтного расписания)
void saveRelaySettings() {
    for (int i = 0; i < 4; i++) {
        String rKey = "rNm" + String(i);
        preferences.putString(rKey.c_str(), relayNames[i]);
    }
    preferences.putBytes("relCtrl", controlType, sizeof(controlType));
    preferences.putBytes("relSetP", setPoint, sizeof(setPoint));
    preferences.putBytes("relHyst", hysteresis, sizeof(hysteresis));
    Serial.println("Relay settings saved!");
}

// Отдельно — тяжёлый массив расписания реле (48 интервалов x 4 канала).
// Пишется только когда реально редактируют страницу /schedule, а не на
// каждое изменение других настроек реле.
void saveRelaySchedule() {
    preferences.putBytes("relSched", schedule, sizeof(schedule));
    Serial.println("Relay schedule saved!");
}

// Имена дозаторов + время срабатывания + длительность (без дней недели)
void saveDoserSettings() {
    for (int i = 0; i < 4; i++) {
        String dKey = "dNm" + String(i);
        preferences.putString(dKey.c_str(), doserNames[i]);
    }
    preferences.putBytes("dRunT", fertMotorRunTime, sizeof(fertMotorRunTime));
    preferences.putBytes("dEn", fertEnabled, sizeof(fertEnabled));
    preferences.putBytes("dTrgH", fertTriggerHour, sizeof(fertTriggerHour));
    preferences.putBytes("dTrgM", fertTriggerMinute, sizeof(fertTriggerMinute));
    preferences.putBytes("dDays", fertDays, sizeof(fertDays));
    Serial.println("Doser settings saved!");
}

// Дни недели дозаторов - отдельно, т.к. сейчас не меняется из веб-интерфейса
void saveDoserSchedule() {
    preferences.putBytes("dDays", fertDays, sizeof(fertDays));
    Serial.println("Doser schedule saved!");
}

void saveFeederSettings() {
    preferences.putUChar("f1H", feed1Hour);
    preferences.putUChar("f1M", feed1Minute);
    preferences.putUChar("f2H", feed2Hour);
    preferences.putUChar("f2M", feed2Minute);
    preferences.putUInt("fDur", feedDuration);
    Serial.println("Feeder settings saved!");
}

// Полное сохранение — оставлено для редких случаев (например, сброс к
// заводским или служебные сценарии). НЕ вызывать на каждое мелкое изменение!
void saveAllSettings() {
    saveSystemSettings();
    saveFanSettings();
    saveLightBrightness();
    saveMoonBrightness();
    saveLightTimers();
    saveRelaySettings();
    saveRelaySchedule();
    saveDoserSettings();
    saveDoserSchedule();
    saveFeederSettings();
    Serial.println("All settings saved to NVS!");
}
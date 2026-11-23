#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Wire.h> 

// Подключаем заголовочные файлы наших модулей
#include "memory.h"   
#include "display.h"  
#include "lighting.h"
#include "temperature.h"
#include "relays.h"
#include "doser.h"
#include "fan.h"
#include "feeder.h"
#include "espnow_sync.h"
#include "web_server.h"
#include "mqtt_service.h"

// Переменные времени
const char* ntpServer = "pool.ntp.org";
int currentHour = 12, currentMinute = 0, currentSecond = 0, currentWeekday = 0;

unsigned long lastSyncTime = 0;
unsigned long lastEspNowSync = 0;

// Объявления функций
void syncTime();

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Starting Aquarium System (Master) ---");

    // 1. Инициализация шины I2C (Обязательно ПЕРЕД дисплеем и модулями)
    Wire.begin(21, 22);
    Wire.setClock(400000); 


    Wire.setTimeOut(50); // мс

    Serial.println("--- I2C Scan ---");
    int foundDevices = 0;
    for (byte addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        byte error = Wire.endTransmission();
        if (error == 0) {
            Serial.printf("I2C device found at 0x%02X\n", addr);
            foundDevices++;
        }
    }
    if (foundDevices == 0) {
        Serial.println("WARNING: No I2C devices found! Check wiring/pull-ups.");
    }
    Serial.println("--- I2C Scan done ---");

    // 2. Инициализация памяти NVS (загрузит все массивы, SSID и пароли)
    initMemory();

    // 3. Инициализация дисплея (внутри настраивается пин кнопки джойстика)
    initDisplay();

    // 3.1 Инициализируем аппаратные часы ESP32, чтобы они "тикали" с 12:00 
    // даже без подключения к интернету
    struct tm t = {0};
    t.tm_year = 126; // Ставим 2026 год
    t.tm_mon = 0;
    t.tm_mday = 1;
    t.tm_hour = 12;  // Стартуем с 12:00
    t.tm_min = 0;
    t.tm_sec = 0;
    time_t timeSinceEpoch = mktime(&t);
    struct timeval tv = { .tv_sec = (long)timeSinceEpoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    // 4. Инициализация аппаратных модулей
    initLighting();
    initTemperatureSensors();
    initRelays();
    initDosers();
    initFan();
    initFeeder();

    // 5. Подключение к Wi-Fi (Клиент)
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    Serial.print("Connecting to WiFi");
    
    unsigned long startAttemptTime = millis();
    // Ждем подключения максимум 10 секунд
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
        delay(500);
        Serial.print(".");
    }

    // Проверяем результат
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected to WiFi!");
        syncTime(); // Синхронизируем NTP-время
    } else {
        Serial.println("\nFailed to connect. Starting Access Point (AP)...");
        WiFi.mode(WIFI_AP); 
        WiFi.softAP("AquaOS", "siliconforge.cc"); 
        
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
    }

    // 6. Инициализация ESP-NOW и веб-сервера
    initESPNOW();
    initWebServer();
}

void loop() {
    unsigned long currentMillis = millis();
    processRemoteCommands();

    // 1. Обновляем локальное время каждую секунду

    if (currentMillis - lastSyncTime >= 1000) {
        lastSyncTime = currentMillis;
        
        // Читаем время напрямую, игнорируя защиту ESP32 на проверку 2016 года
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        
        currentHour = timeinfo.tm_hour;
        currentMinute = timeinfo.tm_min;
        currentSecond = timeinfo.tm_sec;
        currentWeekday = timeinfo.tm_wday; // 0 - Вс, 1 - Пн, ... 6 - Сб
    }
    // 2. Опрос асинхронных датчиков и логики
    // Таймер для температуры
    static unsigned long lastTempUpdate = 0;
    if (currentMillis - lastTempUpdate >= 2000) {
        lastTempUpdate = currentMillis;
        updateTemperatures(); 
    }

    // Логика вентилятора дергает I2C ЦАП - незачем делать это на каждой
    // итерации loop() (сотни раз в секунду), достаточно нескольких раз в
    // секунду. Так шина I2C меньше занята и меньше конкурирует с LCD/PWM.
    static unsigned long lastFanUpdate = 0;
    if (currentMillis - lastFanUpdate >= 150) {
        lastFanUpdate = currentMillis;
        updateFanLogic();         // ПИД управление вентилятором
    }

    handleSmoothTransition(); // Рассветы и закаты
    
    // 3. Проверка расписаний (вызывается раз в секунду)
    static unsigned long lastScheduleCheck = 0;
    if (currentMillis - lastScheduleCheck >= 1000) {
        lastScheduleCheck = currentMillis;
        
        checkLightSchedule(currentHour, currentMinute);
        updateRelays(tempWater, currentHour, currentMinute);
    }

    // Проверяем насосы в каждом проходе loop(), чтобы отключать их по millis()
    // с точностью, необходимой для длительности от 0,1 секунды.
    handleDosers(currentHour, currentMinute, currentSecond, currentWeekday);
    
    updateFeeder(currentHour, currentMinute);
    // Обработка веб-сервера
    handleClientWebServer();
    mqttServiceLoop();

    // 4. Отрисовка дисплея и опрос локального джойстика
    // Функция updateDisplay() внутри себя вызывает getJoystickAction() 
    // и полностью управляет навигацией по меню
    updateDisplay();

    // 5. Сбор и отправка данных по ESP-NOW на модуль в крышке
    if (currentMillis -lastEspNowSync  >= 1000) {
        lastEspNowSync = currentMillis;
        
        displayData.hour = currentHour;
        displayData.minute = currentMinute;
        displayData.second = currentSecond;
        displayData.tempWater = tempWater;
        displayData.tempHeatsink = tempHeatsink;
        displayData.isFanOn = isFanOn;
        displayData.isMoonlight = (isMoonlightEnabled && !isLightOn);
        displayData.isDayPause = (currentLightState == PAUSE);
        displayData.isLightOn = isLightOn;
        displayData.wifiChannel = WiFi.channel();
        
        for(int i = 0; i < 5; i++) {
           
            displayData.brightness[i] = currentBrightness[i];
        }

        //Serial.printf("HEARTBEAT TX ch=%u t=%02d:%02d:%02d water=%.1f heatsink=%.1f\n",
                    //  WiFi.channel(), currentHour, currentMinute, currentSecond,
                    //  tempWater, tempHeatsink);
        sendDataToDisplay();
    }
}

// Запрос времени
void syncTime() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

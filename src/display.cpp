#include "display.h"
#include <Wire.h>
#include <WiFi.h>
#include <LiquidCrystal_I2C.h>
#include <sys/time.h> // Для обновления времени RTC

// Подключаем ваши модули
#include "memory.h"
#include "espnow_sync.h" 
#include "relays.h"      
#include "fan.h"         
#include "lighting.h"  
#include "feeder.h" 
#include "doser.h"       // <-- Пригодится для следующего меню  
#include "temperature.h" // <-- Для tempWater на главном экране

extern int currentHour;
extern int currentMinute;

LiquidCrystal_I2C lcd(0x27, 20, 4);

#define JOY_X 32
#define JOY_Y 33
#define JOY_SW 25

ScreenState currentScreen = SCREEN_MAIN;
unsigned long lastDisplayUpdate = 0;
unsigned long lastJoystickActivity = 0;

const unsigned long backlightTimeout = 300000; 
const unsigned long menuTimeout = 15000;      
bool isBacklightOn = true;
bool settingsChanged = false;                 
static bool filterNotFoundWarningActive = false;
static unsigned long filterNotFoundWarningStartedAt = 0;
static const unsigned long filterNotFoundWarningDurationMs = 5000;

// --- Настройки навигации ---
const int totalMenuItems = 10; 
int currentMenuItem = 0;      
int menuScrollIndex = 0;      

const char* menuItems[totalMenuItems] = {
    "1. Light PWM",
    "2. Moon PWM",
    "3. Light Schedule",
    "4. Day Pause",
    "5. Fan Settings",
    "6. Relays Setup", 
    "7. Dosers Setup",
    "8. Feeder Setup",
    "9. System Time",
    "10. Exit"
};

// Имена каналов света
const char* lightNames[5] = {"Red", "Blue", "White 1", "White 2", "White 3"};

// Короткие 2-буквенные подписи каналов для главного экрана (выравнивание под
// цифры требует, чтобы КАЖДАЯ подпись была ровно из 2 символов - замените
// "RD"/"BL" на те буквы, которые вы реально использовали при переименовании).
const char* channelShortLabels[5] = {"RD", "BL", "W1", "W2", "W3"};

int subMenuIndex = 0; 
int selectedChannel = 0; 
int schedInterval = 0; 

// Временные переменные для настройки часов
int tempHour = 0;
int tempMinute = 0;
int tempGmt = 0;

byte circleEmpty[8] = {0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110, 0b00000, 0b00000};
byte circleFilled[8] = {0b00000, 0b01110, 0b11111, 0b11111, 0b11111, 0b01110, 0b00000, 0b00000};

// --- Кадры анимации вентилятора ---
byte fanFrame1[8] = {0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b00000, 0b00000}; // Лопасти крестиком (X)
byte fanFrame2[8] = {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000, 0b00000}; // Лопасти плюсиком (+)

// Состояние мигания двоеточия в часах на главном экране 1.
// Колонка на LCD, где стоит двоеточие в строке "R  B  W1 W2 W3 HH:MM" - 17-я (0-based).
bool colonVisible = true;
const int TIME_COLON_COL = 17;

enum JoyAction { NONE, UP, DOWN, LEFT, RIGHT, CLICK };

JoyAction getJoystickAction() {
    // ВАЖНО: раньше направления (X/Y) и клик кнопки использовали ОДИН общий
    // таймер lastActionTime. Аналоговый джойстик почти никогда не сидит
    // идеально в центре АЦП - шум на осях X/Y постоянно ложно триггерил
    // UP/DOWN/LEFT/RIGHT и откатывал общий таймер, из-за чего клик почти
    // никогда не попадал в нужное окно (>200мс). Именно это вызывало
    // "нужно нажать 20 раз, чтобы попасть в меню". Теперь у направлений и
    // клика РАЗНЫЕ таймеры, они не мешают друг другу.
    static unsigned long lastMoveTime = 0;
    static unsigned long lastClickTime = 0;
    static bool buttonWasPressed = false;

    // Дополнительно: нормальный аппаратный дебаунс самой кнопки по
    // стабильности сигнала (а не просто "поймали LOW один раз") - защищает
    // от дребезга контактов джойстика.
    static bool lastRawButtonState = false;
    static unsigned long lastButtonChangeTime = 0;

    int x = analogRead(JOY_X);
    int y = analogRead(JOY_Y);
    bool rawButtonPressed = (digitalRead(JOY_SW) == LOW);
    JoyAction action = NONE;

    if (millis() - lastMoveTime > 250) {
        // Оси X и Y поменяны местами (джойстик развёрнут на 90°).
        if (x > 3000) action = UP;
        else if (x < 1000) action = DOWN;
        else if (y > 3000) action = RIGHT;
        else if (y < 1000) action = LEFT;
        if (action != NONE) lastMoveTime = millis();
    }

    // Дебаунс кнопки: считаем сигнал достоверным только после того, как он
    // не менялся 30 мс подряд (гасит дребезг контактов).
    if (rawButtonPressed != lastRawButtonState) {
        lastButtonChangeTime = millis();
        lastRawButtonState = rawButtonPressed;
    }
    bool buttonPressed = rawButtonPressed && (millis() - lastButtonChangeTime > 30);

    if (buttonPressed && !buttonWasPressed && (millis() - lastClickTime > 200)) {
        action = CLICK;
        lastClickTime = millis();
        buttonWasPressed = true;
    } else if (!buttonPressed) {
        buttonWasPressed = false;
    }

    if (action != NONE || buttonPressed) {
        lastJoystickActivity = millis();
        if (!isBacklightOn) {
            lcd.backlight();
            isBacklightOn = true;
            return NONE; 
        }
    }
    if (isBacklightOn && (millis() - lastJoystickActivity > backlightTimeout)) {
        lcd.noBacklight();
        isBacklightOn = false;
        currentScreen = SCREEN_MAIN; 
    }
    return action;
}

void initDisplay() {
    pinMode(JOY_SW, INPUT_PULLUP);
    lcd.init();
    lcd.backlight();
    lcd.createChar(1, circleEmpty);
    lcd.createChar(2, circleFilled);
  //  lcd.createChar(3, fanFrame1); // Загружаем кадр 1
   // lcd.createChar(4, fanFrame2); // Загружаем кадр 2
}

// ================= ОТРИСОВКА ЭКРАНОВ =================

// --- ЭКРАН ВЕНТИЛЯТОРА ---
void drawFanMenu() {
    char buf[21];
    lcd.setCursor(0, 0); lcd.print("--- Fan Settings ---");
    
    // Строка 1: On и Max
    if (subMenuIndex == 0) snprintf(buf, sizeof(buf), ">On:%4.1f  Max:%4.1f ", fanOnTemp, maxFanTemp);
    else if (subMenuIndex == 1) snprintf(buf, sizeof(buf), " On:%4.1f >Max:%4.1f ", fanOnTemp, maxFanTemp);
    else snprintf(buf, sizeof(buf), " On:%4.1f  Max:%4.1f ", fanOnTemp, maxFanTemp);
    lcd.setCursor(0, 1); lcd.print(buf);

    // Строка 2: Start % и Hys
    if (subMenuIndex == 2) snprintf(buf, sizeof(buf), ">Min:%3d%%  Hys:%4.1f ", fanStartLevel, fanHysteresis);
    else if (subMenuIndex == 3) snprintf(buf, sizeof(buf), " Min:%3d%% >Hys:%4.1f ", fanStartLevel, fanHysteresis);
    else snprintf(buf, sizeof(buf), " Min:%3d%%  Hys:%4.1f ", fanStartLevel, fanHysteresis);
    lcd.setCursor(0, 2); lcd.print(buf);

    // Строка 3: Кнопка сохранения
    lcd.setCursor(0, 3);
    lcd.print(subMenuIndex == 4 ? ">Save & Exit        " : " Save & Exit        ");
}

// Определяем текущий режим освещения (день/ночь/пауза) на основе уже
// доступных глобальных переменных - без обращения к внутреннему enum
// LightState из lighting.cpp (он там объявлен без extern и недоступен здесь).
const char* getLightModeLabel() {
    switch (currentLightState) {
        case DAY:           return "DAY";
        case SUNRISE:       return "SUNUP";  // Рассвет (до 6 символов)
        case SUNSET:        return "SUNDWN"; // Закат
        case PAUSE:         
        case PAUSE_SUNRISE: 
        case PAUSE_SUNSET:  return "PAUSE";
        case MOON:          return "MOON";
        case OFF:           
        default:            return "NIGHT";
    }
}

// --- ГЛАВНЫЙ ЭКРАН 1: яркость каналов, время, режим, температура воды, сеть ---
void drawMainScreen() {
    char buf[21];

    // Строка 0: заголовки каналов света + время
    snprintf(buf, sizeof(buf), "%s %s %s %s %s %02d%c%02d",
             channelShortLabels[0], channelShortLabels[1], channelShortLabels[2],
             channelShortLabels[3], channelShortLabels[4],
             displayData.hour, colonVisible ? ':' : ' ', displayData.minute);
    lcd.setCursor(0, 0); lcd.print(buf);

    // Строка 1: значения яркости + температура воды (%4.1fC)
    lcd.setCursor(0, 1);
    snprintf(buf, sizeof(buf), "%2d %2d %2d %2d %2d %4.1fC",
             min((int)displayData.brightness[0], 99), min((int)displayData.brightness[1], 99),
             min((int)displayData.brightness[2], 99), min((int)displayData.brightness[3], 99),
             min((int)displayData.brightness[4], 99), tempWater);
    lcd.print(buf);

     // Строка 2: статус подключения + температура LED (ровно 20 символов)
    // "Net: " (5) + статус сети (10) + температура (5) = 20 символов
    bool isAP = (WiFi.getMode() == WIFI_AP);
    snprintf(buf, sizeof(buf), "Net: %-10s%4.1fC", isAP ? "AP" : "WiFi", tempHeatsink);
    lcd.setCursor(0, 2); 
    lcd.print(buf);

    // Строка 3: режим освещения + 2 пробела для затирки под звездочку (20 символов)
    snprintf(buf, sizeof(buf), "Light: %-6s       ", getLightModeLabel());
    lcd.setCursor(0, 3); lcd.print(buf);
}

// --- ГЛАВНЫЙ ЭКРАН 2: статус и режим реле, IP-адрес устройства ---
void drawRelayStatusScreen() {
    char buf[21];

    lcd.setCursor(0, 0);
    lcd.print("--- Relays ---      ");

    // Строка 1: имена реле (по 4 символа на каждое)
    lcd.setCursor(0, 1);
    for (int i = 0; i < 4; i++) {
        snprintf(buf, sizeof(buf), "%-4.4s ", relayNames[i].c_str());
        lcd.print(buf);
    }

    // Строка 2: статус (значок вкл/выкл) + режим (T - термостат, S - расписание)
    lcd.setCursor(0, 2);
    for (int i = 0; i < 4; i++) {
        lcd.write(relayState[i] ? 2 : 1);
        lcd.print(controlType[i] == 0 ? "T   " : "S   ");
    }

    // Строка 3: IP-адрес устройства (в режиме AP или как клиент Wi-Fi)
    bool isAP = (WiFi.getMode() == WIFI_AP);
    IPAddress ip = isAP ? WiFi.softAPIP() : WiFi.localIP();
    snprintf(buf, sizeof(buf), "IP: %-16s", ip.toString().c_str());
    lcd.setCursor(0, 3); lcd.print(buf);
}

void drawRootMenu() {
    lcd.setCursor(0, 0); lcd.print("--- SYSTEM MENU ----");
    for (int i = 0; i < 3; i++) {
        int itemIndex = menuScrollIndex + i;
        if (itemIndex >= totalMenuItems) break;
        lcd.setCursor(0, i + 1);
        if (itemIndex == currentMenuItem) lcd.print(">"); else lcd.print(" ");
        lcd.print(menuItems[itemIndex]);
        for(int s = strlen(menuItems[itemIndex]) + 1; s < 20; s++) lcd.print(" ");
    }
}

// --- ЭКРАН СВЕТА (PWM) ---
void drawLightPwmMenu() {
    char buf[21];

    // Строка 0: все каналы (4-символьные колонки: 2 буквы + 2 пробела)
    snprintf(buf, sizeof(buf), "%-4s%-4s%-4s%-4s%-4s",
             channelShortLabels[0], channelShortLabels[1], channelShortLabels[2],
             channelShortLabels[3], channelShortLabels[4]);
    lcd.setCursor(1, 0);
    lcd.print(buf);

    // Строка 1: значения яркости всех каналов
    snprintf(buf, sizeof(buf), "%3d %3d %3d %3d %3d ",
             brightnessSettings[0], brightnessSettings[1], brightnessSettings[2],
             brightnessSettings[3], brightnessSettings[4]);
    lcd.setCursor(0, 1);
    lcd.print(buf);

    // Строка 3: полное имя выбранного канала (для наглядности)
    snprintf(buf, sizeof(buf), "%-20s", lightNames[selectedChannel]);
    lcd.setCursor(0, 3);
    lcd.print(buf);

    // Строка 2: курсор-указатель под выбранным каналом
    char cursorLine[21];
    for (int i = 0; i < 20; i++) cursorLine[i] = ' ';
    cursorLine[20] = '\0';
    int pos = constrain(selectedChannel * 4 + 1, 0, 19);
    cursorLine[pos] = '^';
    lcd.setCursor(0, 2);
    lcd.print(cursorLine);
}

// --- ЭКРАН ВРЕМЕНИ ---
void drawTimeMenu() {
    char buf[21];
    lcd.setCursor(0, 0); lcd.print("--- System Time ----");
    
    lcd.setCursor(0, 1);
    if (subMenuIndex == 0) {
        snprintf(buf, sizeof(buf), ">H: %02d   M: %02d      ", tempHour, tempMinute);
    } else if (subMenuIndex == 1) {
        snprintf(buf, sizeof(buf), " H: %02d  >M: %02d      ", tempHour, tempMinute);
    } else {
        snprintf(buf, sizeof(buf), " H: %02d   M: %02d      ", tempHour, tempMinute);
    }
    lcd.print(buf);

    lcd.setCursor(0, 2);
    snprintf(buf, sizeof(buf), "%cGMT:  %+d         ", (subMenuIndex == 2 ? '>' : ' '), tempGmt);
    lcd.print(buf);

    lcd.setCursor(0, 3);
    lcd.print(subMenuIndex == 3 ? ">Save & Exit        " : " Save & Exit        ");
}

// --- ЭКРАНЫ ДЛЯ РЕЛЕ ---
void drawRelaysMenu() {
    lcd.setCursor(0, 0); lcd.print("--- Relay Config ---");
    lcd.setCursor(0, 1);
    lcd.print(subMenuIndex == 0 ? ">Relay: " : " Relay: ");
    lcd.print(selectedChannel + 1); lcd.print("    ");
    lcd.setCursor(0, 2);
    lcd.print(subMenuIndex == 1 ? ">Mode: " : " Mode: ");
    lcd.print(controlType[selectedChannel] == 0 ? "Thermostat" : "Schedule  ");
    lcd.setCursor(0, 3); lcd.print(" Click to setup...");
}

void drawRelayThermMenu() {
    lcd.setCursor(0, 0); lcd.print("--- Thermostat ---  ");
    lcd.setCursor(0, 1);
    lcd.print(subMenuIndex == 0 ? ">Target t: " : " Target t: ");
    lcd.print(setPoint[selectedChannel], 1); lcd.print("C  ");
    lcd.setCursor(0, 2);
    lcd.print(subMenuIndex == 1 ? ">Hysteres: " : " Hysteres: ");
    lcd.print(hysteresis[selectedChannel], 1); lcd.print("C  ");
    lcd.setCursor(0, 3);
    lcd.print(subMenuIndex == 2 ? ">Save & Exit        " : " Save & Exit        ");
}

void drawRelaySchedMenu() {
    lcd.setCursor(0, 0); 
    lcd.print("- Relay "); lcd.print(selectedChannel + 1); lcd.print(" Timer -  ");
    int h = schedInterval / 2;
    int m = (schedInterval % 2) * 30;
    lcd.setCursor(0, 1);
    char buf[21];
    snprintf(buf, sizeof(buf), "%cTime: %02d:%02d  [%s]", 
            (subMenuIndex == 0 ? '>' : ' '), h, m, 
            (schedule[selectedChannel][schedInterval] ? "ON " : "OFF"));
    lcd.print(buf);
    lcd.setCursor(0, 2);
    lcd.print(subMenuIndex == 1 ? ">Save & Exit        " : " Save & Exit        ");
    lcd.setCursor(0, 3); lcd.print(" <-> Time | CLK: ON ");
}

// --- ЭКРАН НОЧНОЙ ПОДСВЕТКИ (MOON) ---
void drawMoonPwmMenu() {
    char buf[21];

    // Строка 0: заголовки каналов с отступами
    snprintf(buf, sizeof(buf), " %-3s %-3s %-3s %-3s %-3s",
             channelShortLabels[0], channelShortLabels[1], channelShortLabels[2],
             channelShortLabels[3], channelShortLabels[4]);
    lcd.setCursor(0, 0);
    lcd.print(buf);

    // Строка 1: значения яркости всех каналов луны
    snprintf(buf, sizeof(buf), "%3d %3d %3d %3d %3d ",
             moonlightSettings[0], moonlightSettings[1], moonlightSettings[2],
             moonlightSettings[3], moonlightSettings[4]);
    lcd.setCursor(0, 1);
    lcd.print(buf);

    // Строка 2: курсор-указатель (прячется, если выбран режим ВКЛ/ВЫКЛ)
    char cursorLine[21];
    for (int i = 0; i < 20; i++) cursorLine[i] = ' ';
    cursorLine[20] = '\0';
    if (selectedChannel < 5) {
        int pos = constrain(selectedChannel * 4 + 1, 0, 19);
        cursorLine[pos] = '^';
    }
    lcd.setCursor(0, 2);
    lcd.print(cursorLine);

    // Строка 3: статус луны и имя канала, ИЛИ активный тумблер
    if (selectedChannel < 5) {
        snprintf(buf, sizeof(buf), "Moon:%-3s Ch:%-7s", isMoonlightEnabled ? "ON" : "OFF", lightNames[selectedChannel]);
    } else {
        snprintf(buf, sizeof(buf), "> Moon Mode: %-3s    ", isMoonlightEnabled ? "ON" : "OFF");
    }
    lcd.setCursor(0, 3);
    lcd.print(buf);
}

// --- ЭКРАН ТАЙМЕРОВ СВЕТА И РАССВЕТА ---
void drawLightSchedMenu() {
    lcd.setCursor(0, 0); lcd.print("--- Light Timers ---");
    char buf[21];
    
    lcd.setCursor(0, 1);
    if (subMenuIndex == 0) snprintf(buf, sizeof(buf), " ON:  >%02d:%02d       ", lightOnHour, lightOnMinute);
    else if (subMenuIndex == 1) snprintf(buf, sizeof(buf), " ON:   %02d>%02d       ", lightOnHour, lightOnMinute);
    else snprintf(buf, sizeof(buf), " ON:   %02d:%02d       ", lightOnHour, lightOnMinute);
    lcd.print(buf);
    
    lcd.setCursor(0, 2);
    if (subMenuIndex == 2) snprintf(buf, sizeof(buf), " OFF: >%02d:%02d       ", lightOffHour, lightOffMinute);
    else if (subMenuIndex == 3) snprintf(buf, sizeof(buf), " OFF:  %02d>%02d       ", lightOffHour, lightOffMinute);
    else snprintf(buf, sizeof(buf), " OFF:  %02d:%02d       ", lightOffHour, lightOffMinute);
    lcd.print(buf);
    
    lcd.setCursor(0, 3);
    // Слегка сократил "min", чтобы точно влезло в 20 символов экрана
    snprintf(buf, sizeof(buf), "%cFade: %02dm [CLK=OK]", (subMenuIndex == 4 ? '>' : ' '), sunriseDuration);
    lcd.print(buf);
}

// --- ЭКРАН ДНЕВНОЙ ПАУЗЫ ---
void drawDayPauseMenu() {
    lcd.setCursor(0, 0); lcd.print("--- Day Pause ------");
    char buf[21];
    
    lcd.setCursor(0, 1);
    if (dayPauseStartHour == 25) {
        if (subMenuIndex == 0)      snprintf(buf, sizeof(buf), " Start: >OFF        ");
        else if (subMenuIndex == 1) snprintf(buf, sizeof(buf), " Start:  OFF>       ");
        else                        snprintf(buf, sizeof(buf), " Start:  OFF        ");
    } else {
        if (subMenuIndex == 0)      snprintf(buf, sizeof(buf), " Start: >%02d:%02d    ", dayPauseStartHour, dayPauseStartMinute);
        else if (subMenuIndex == 1) snprintf(buf, sizeof(buf), " Start:  %02d>%02d    ", dayPauseStartHour, dayPauseStartMinute);
        else                        snprintf(buf, sizeof(buf), " Start:  %02d:%02d    ", dayPauseStartHour, dayPauseStartMinute);
    }
    lcd.print(buf);
    
    lcd.setCursor(0, 2);
    if (dayPauseEndHour == 25) {
        if (subMenuIndex == 2)      snprintf(buf, sizeof(buf), " End:   >OFF        ");
        else if (subMenuIndex == 3) snprintf(buf, sizeof(buf), " End:    OFF>       ");
        else                        snprintf(buf, sizeof(buf), " End:    OFF        ");
    } else {
        if (subMenuIndex == 2)      snprintf(buf, sizeof(buf), " End:   >%02d:%02d    ", dayPauseEndHour, dayPauseEndMinute);
        else if (subMenuIndex == 3) snprintf(buf, sizeof(buf), " End:    %02d>%02d    ", dayPauseEndHour, dayPauseEndMinute);
        else                        snprintf(buf, sizeof(buf), " End:    %02d:%02d    ", dayPauseEndHour, dayPauseEndMinute);
    }
    lcd.print(buf);
    
    lcd.setCursor(0, 3);
    lcd.print(" [CLK=OK]           ");
}

// --- ЭКРАН ДОЗАТОРОВ ---
void drawDosersMenu() {
    lcd.setCursor(0, 0); lcd.print("--- Doser Config ---");
    
    lcd.setCursor(0, 1);
    char buf[21];
    snprintf(buf, sizeof(buf), "%cPump:%d Fert:%c%s ",
        subMenuIndex == 0 ? '>' : ' ',
        selectedChannel + 1,
        subMenuIndex == 4 ? '>' : ' ',
        fertEnabled[selectedChannel] ? "ON" : "OFF");
    lcd.print(buf);

    lcd.setCursor(0, 2);
    if (subMenuIndex == 1) snprintf(buf, sizeof(buf), " Time: >%02d:%02d      ", fertTriggerHour[selectedChannel], fertTriggerMinute[selectedChannel]);
    else if (subMenuIndex == 2) snprintf(buf, sizeof(buf), " Time:  %02d>%02d      ", fertTriggerHour[selectedChannel], fertTriggerMinute[selectedChannel]);
    else snprintf(buf, sizeof(buf), " Time:  %02d:%02d      ", fertTriggerHour[selectedChannel], fertTriggerMinute[selectedChannel]);
    lcd.print(buf);

    lcd.setCursor(0, 3);
    if (subMenuIndex >= 5) {
        static const char* dayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int day = subMenuIndex - 5;
        snprintf(buf, sizeof(buf), ">Day %s: %s", dayNames[day], fertDays[selectedChannel][day] ? "ON" : "OFF");
    } else {
        unsigned long runTime = fertMotorRunTime[selectedChannel];
        if (runTime < 1) runTime = 1;
        if (runTime > 300) runTime = 300;
        snprintf(buf, sizeof(buf), "%cRun: %lu.%lus [CLK]",
            subMenuIndex == 3 ? '>' : ' ',
            runTime / 10,
            runTime % 10);
    }
    lcd.print(buf);
}

// --- ЭКРАН КОРМУШКИ ---
void drawFeederMenu() {
    lcd.setCursor(0, 0); lcd.print("--- Auto Feeder ----");
    char buf[21];
    
    lcd.setCursor(0, 1);
    if (feed1Hour == 25) {
        if (subMenuIndex == 0)      snprintf(buf, sizeof(buf), " F1: >OFF           ");
        else if (subMenuIndex == 1) snprintf(buf, sizeof(buf), " F1:  OFF>          ");
        else                        snprintf(buf, sizeof(buf), " F1:  OFF           ");
    } else {
        if (subMenuIndex == 0)      snprintf(buf, sizeof(buf), " F1: >%02d:%02d        ", feed1Hour, feed1Minute);
        else if (subMenuIndex == 1) snprintf(buf, sizeof(buf), " F1:  %02d>%02d        ", feed1Hour, feed1Minute);
        else                        snprintf(buf, sizeof(buf), " F1:  %02d:%02d        ", feed1Hour, feed1Minute);
    }
    lcd.print(buf);

    lcd.setCursor(0, 2);
    if (feed2Hour == 25) {
        if (subMenuIndex == 2)      snprintf(buf, sizeof(buf), " F2: >OFF           ");
        else if (subMenuIndex == 3) snprintf(buf, sizeof(buf), " F2:  OFF>          ");
        else                        snprintf(buf, sizeof(buf), " F2:  OFF           ");
    } else {
        if (subMenuIndex == 2)      snprintf(buf, sizeof(buf), " F2: >%02d:%02d        ", feed2Hour, feed2Minute);
        else if (subMenuIndex == 3) snprintf(buf, sizeof(buf), " F2:  %02d>%02d        ", feed2Hour, feed2Minute);
        else                        snprintf(buf, sizeof(buf), " F2:  %02d:%02d        ", feed2Hour, feed2Minute);
    }
    lcd.print(buf);

    lcd.setCursor(0, 3);
    float durSec = feedDuration / 1000.0;
    char settingsLine[21];
    snprintf(settingsLine, sizeof(settingsLine), "%cD:%.1fs %cFP:%s",
        subMenuIndex == 4 ? '>' : ' ',
        durSec,
        subMenuIndex == 5 ? '>' : ' ',
        feedFilterPauseEnabled ? "ON" : "OFF");
    snprintf(buf, sizeof(buf), "%-20s", settingsLine);
    lcd.print(buf);
}

// ================= ЛОГИКА УПРАВЛЕНИЯ =================

void updateDisplay() {
    JoyAction action = getJoystickAction();
    if (!isBacklightOn) return; 

    // --- ЛОГИКА АВТОВЫХОДА И АВТОСОХРАНЕНИЯ (10 сек) ---
    if (currentScreen != SCREEN_MAIN && currentScreen != SCREEN_MAIN2 && (millis() - lastJoystickActivity > menuTimeout)) {
        if (settingsChanged) {
            saveAllSettings();
            settingsChanged = false;
        }
        currentScreen = SCREEN_MAIN; 
        lcd.clear();
        return; 
    }

    bool needsRedraw = false;

    switch (currentScreen) {
        case SCREEN_MAIN:
            if (action == CLICK) {
                currentScreen = SCREEN_MENU_ROOT;
                currentMenuItem = 0; menuScrollIndex = 0;
                settingsChanged = false; 
                lcd.clear();
            } else if (action == LEFT || action == RIGHT) {
                currentScreen = SCREEN_MAIN2;
                lcd.clear();
            } else if (action == UP) {
                activateMaxBrightness();
                needsRedraw = true; 
            } else if (action == DOWN) {
                if (!toggleFilterMaintenance()) {
                    filterNotFoundWarningActive = true;
                    filterNotFoundWarningStartedAt = millis();
                }
                needsRedraw = true;
            }
            break;

        case SCREEN_MAIN2:
            if (action == CLICK) {
                currentScreen = SCREEN_MENU_ROOT;
                currentMenuItem = 0; menuScrollIndex = 0;
                settingsChanged = false;
                lcd.clear();
            } else if (action == LEFT || action == RIGHT) {
                currentScreen = SCREEN_MAIN;
                lcd.clear();
            } else if (action == UP) {
                activateMaxBrightness();
                needsRedraw = true; 
            } else if (action == DOWN) {
                if (!toggleFilterMaintenance()) {
                    filterNotFoundWarningActive = true;
                    filterNotFoundWarningStartedAt = millis();
                }
                needsRedraw = true;
            }
            break;

        case SCREEN_MENU_ROOT:
            if (action == DOWN) {
                currentMenuItem++;
                if (currentMenuItem >= totalMenuItems) currentMenuItem = 0; 
                if (currentMenuItem >= menuScrollIndex + 3) menuScrollIndex++;
                else if (currentMenuItem == 0) menuScrollIndex = 0;
                needsRedraw = true;
            } else if (action == UP) {
                currentMenuItem--;
                if (currentMenuItem < 0) {
                    currentMenuItem = totalMenuItems - 1;
                    menuScrollIndex = totalMenuItems - 3; 
                }
                if (currentMenuItem < menuScrollIndex) menuScrollIndex--;
                needsRedraw = true;
            } else if (action == CLICK) {
                subMenuIndex = 0; selectedChannel = 0; 
                
                if (currentMenuItem == 0) currentScreen = SCREEN_LIGHT_PWM;
                else if (currentMenuItem == 1) currentScreen = SCREEN_MOON_PWM;
                else if (currentMenuItem == 2) currentScreen = SCREEN_LIGHT_SCHED;
                else if (currentMenuItem == 3) currentScreen = SCREEN_DAY_PAUSE;
                else if (currentMenuItem == 4) currentScreen = SCREEN_FAN_SET;
                else if (currentMenuItem == 5) currentScreen = SCREEN_RELAYS; 
                else if (currentMenuItem == 6) currentScreen = SCREEN_DOSERS;
                else if (currentMenuItem == 7) currentScreen = SCREEN_FEEDER;
                else if (currentMenuItem == 8) {
                    // Инициализируем временные переменные при входе в меню времени
                    tempHour = currentHour;
                    tempMinute = currentMinute;
                    tempGmt = gmtOffset_sec / 3600;
                    currentScreen = SCREEN_TIME;
                }
                else if (currentMenuItem == 9) {
                    if (settingsChanged) saveAllSettings();
                    settingsChanged = false;
                    currentScreen = SCREEN_MAIN; 
                }
                lcd.clear();
            }
            break;

        // --- ЭКРАН СВЕТА (PWM) ---
        case SCREEN_LIGHT_PWM:
            if (action == LEFT) {
                selectedChannel = (selectedChannel - 1 + 5) % 5;
                needsRedraw = true;
            } else if (action == RIGHT) {
                selectedChannel = (selectedChannel + 1) % 5;
                needsRedraw = true;
            } else if (action == UP) {
                brightnessSettings[selectedChannel] = constrain(brightnessSettings[selectedChannel] + 1, 0, 100);
                if (isLightOn) setPWMOutputs(brightnessSettings);
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) {
                brightnessSettings[selectedChannel] = constrain(brightnessSettings[selectedChannel] - 1, 0, 100);
                if (isLightOn) setPWMOutputs(brightnessSettings);
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK) {
                if (settingsChanged) saveLightBrightness();
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;

        // --- ЭКРАН СИСТЕМНОГО ВРЕМЕНИ ---
        case SCREEN_TIME:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 4; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 4) % 4; needsRedraw = true; }
            else if (action == UP) { // Плюс
                if (subMenuIndex == 0) tempHour = (tempHour + 1) % 24;
                else if (subMenuIndex == 1) tempMinute = (tempMinute + 1) % 60;
                else if (subMenuIndex == 2) tempGmt = constrain(tempGmt + 1, -12, 14);
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) { // Минус
                if (subMenuIndex == 0) tempHour = (tempHour - 1 + 24) % 24;
                else if (subMenuIndex == 1) tempMinute = (tempMinute - 1 + 60) % 60;
                else if (subMenuIndex == 2) tempGmt = constrain(tempGmt - 1, -12, 14);
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK && subMenuIndex == 3) {
                if (settingsChanged) {
                    gmtOffset_sec = tempGmt * 3600;
                    struct tm t = {0};
                    time_t now; time(&now); localtime_r(&now, &t); 
                    t.tm_hour = tempHour; t.tm_min = tempMinute; t.tm_sec = 0;
                    if (t.tm_year < 120) t.tm_year = 126;
                    time_t timeSinceEpoch = mktime(&t);
                    struct timeval tv = { .tv_sec = (long)timeSinceEpoch, .tv_usec = 0 };
                    settimeofday(&tv, NULL); 
                    saveSystemSettings();
                }
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;

 // --- РЕЛЕ И РАСПИСАНИЯ ---
        case SCREEN_RELAYS:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 2; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 2) % 2; needsRedraw = true; }
            else if (action == UP) { // Плюс
                if (subMenuIndex == 0) selectedChannel = (selectedChannel + 1) % 4;
                else controlType[selectedChannel] = 1; 
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) { // Минус
                if (subMenuIndex == 0) selectedChannel = (selectedChannel - 1 + 4) % 4;
                else controlType[selectedChannel] = 0; 
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK) {
                subMenuIndex = 0; 
                if (controlType[selectedChannel] == 0) currentScreen = SCREEN_RELAY_THERM;
                else { currentScreen = SCREEN_RELAY_SCHED; schedInterval = 0; }
                lcd.clear();
            }
            break;

        case SCREEN_RELAY_THERM:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 3; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 3) % 3; needsRedraw = true; }
            else if (action == UP) { // Плюс
                if (subMenuIndex == 0) setPoint[selectedChannel] += 0.5;
                else if (subMenuIndex == 1) hysteresis[selectedChannel] += 0.5;
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) { // Минус
                if (subMenuIndex == 0) setPoint[selectedChannel] -= 0.5;
                else if (subMenuIndex == 1) hysteresis[selectedChannel] -= 0.5;
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK && subMenuIndex == 2) {
                if (settingsChanged) saveRelaySettings();
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;

        case SCREEN_RELAY_SCHED:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 2; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 2) % 2; needsRedraw = true; }
            else if (action == UP) { // Плюс интервал
                if (subMenuIndex == 0) schedInterval = (schedInterval + 1) % 48; 
                needsRedraw = true;
            } else if (action == DOWN) { // Минус интервал
                if (subMenuIndex == 0) schedInterval = (schedInterval - 1 + 48) % 48; 
                needsRedraw = true;
            } else if (action == CLICK) {
                if (subMenuIndex == 0) {
                    schedule[selectedChannel][schedInterval] = !schedule[selectedChannel][schedInterval];
                    settingsChanged = true; needsRedraw = true;
                } else if (subMenuIndex == 1) {
                    if (settingsChanged) saveRelaySchedule();
                    settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
                }
            }
            break;

        // --- ЭКРАН ВЕНТИЛЯТОРА ---
        case SCREEN_FAN_SET:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 5; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 5) % 5; needsRedraw = true; }
            else if (action == UP) { // Плюс
                if (subMenuIndex == 0) fanOnTemp += 0.5;
                else if (subMenuIndex == 1) maxFanTemp += 0.5;
                else if (subMenuIndex == 2) fanStartLevel = constrain(fanStartLevel + 5, 0, 100);
                else if (subMenuIndex == 3) fanHysteresis += 0.5;
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) { // Минус
                if (subMenuIndex == 0) fanOnTemp -= 0.5;
                else if (subMenuIndex == 1) maxFanTemp -= 0.5;
                else if (subMenuIndex == 2) fanStartLevel = constrain(fanStartLevel - 5, 0, 100);
                else if (subMenuIndex == 3) fanHysteresis -= 0.5;
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK && subMenuIndex == 4) { // Клик работает только на кнопке Save
                if (settingsChanged) saveFanSettings();
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;
            
           // --- ЭКРАН НОЧНОЙ ПОДСВЕТКИ ---
        case SCREEN_MOON_PWM:
            if (action == RIGHT) {
                // 6 состояний: 5 каналов (0-4) + 1 тумблер Вкл/Выкл (5)
                selectedChannel = (selectedChannel + 1) % 6; 
                needsRedraw = true;
            } else if (action == LEFT) {
                selectedChannel = (selectedChannel - 1 + 6) % 6;
                needsRedraw = true;
            } else if (action == UP || action == DOWN) {
                if (selectedChannel < 5) { // Меняем яркость канала
                    if (action == UP) {
                        moonlightSettings[selectedChannel] = constrain(moonlightSettings[selectedChannel] + 1, 0, 100);
                    } else {
                        moonlightSettings[selectedChannel] = constrain(moonlightSettings[selectedChannel] - 1, 0, 100);
                    }
                    if (!isLightOn) setPWMOutputs(moonlightSettings); 
                } else { // Меняем статус всей луны (ВКЛ/ВЫКЛ)
                    isMoonlightEnabled = !isMoonlightEnabled;
                    if (!isLightOn) {
                        if (isMoonlightEnabled) setPWMOutputs(moonlightSettings);
                        else turnOffAllChannels(); // Сразу гасим, если отключили ночью
                    }
                }
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK) {
                if (settingsChanged) {
                    saveMoonBrightness();
                    saveLightTimers(); // Сохраняем флаг isMoonlightEnabled
                }
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;

      // --- ЭКРАН ТАЙМЕРОВ СВЕТА И РАССВЕТА ---
        case SCREEN_LIGHT_SCHED:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 5; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 5) % 5; needsRedraw = true; }
            else if (action == UP) { // Плюс
                if (subMenuIndex == 0) lightOnHour = (lightOnHour + 1) % 24;
                else if (subMenuIndex == 1) lightOnMinute = (lightOnMinute + 1) % 60;
                else if (subMenuIndex == 2) lightOffHour = (lightOffHour + 1) % 24;
                else if (subMenuIndex == 3) lightOffMinute = (lightOffMinute + 1) % 60;
                else if (subMenuIndex == 4) sunriseDuration += 5;
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) { // Минус
                if (subMenuIndex == 0) lightOnHour = (lightOnHour - 1 + 24) % 24;
                else if (subMenuIndex == 1) lightOnMinute = (lightOnMinute - 1 + 60) % 60;
                else if (subMenuIndex == 2) lightOffHour = (lightOffHour - 1 + 24) % 24;
                else if (subMenuIndex == 3) lightOffMinute = (lightOffMinute - 1 + 60) % 60;
                else if (subMenuIndex == 4 && sunriseDuration > 0) sunriseDuration -= 5;
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK) {
                if (settingsChanged) saveLightTimers();
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;

        // --- ЭКРАН ДНЕВНОЙ ПАУЗЫ ---
        case SCREEN_DAY_PAUSE:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 4; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 4) % 4; needsRedraw = true; }
            else if (action == UP) { // Плюс
                if (subMenuIndex == 0) {
                    dayPauseStartHour++;
                    if (dayPauseStartHour == 24) dayPauseStartHour = 25; // Переход в OFF
                    else if (dayPauseStartHour > 25) dayPauseStartHour = 0; // Возврат к 0
                }
                else if (subMenuIndex == 1) dayPauseStartMinute = (dayPauseStartMinute + 1) % 60;
                else if (subMenuIndex == 2) {
                    dayPauseEndHour++;
                    if (dayPauseEndHour == 24) dayPauseEndHour = 25;
                    else if (dayPauseEndHour > 25) dayPauseEndHour = 0;
                }
                else if (subMenuIndex == 3) dayPauseEndMinute = (dayPauseEndMinute + 1) % 60;
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) { // Минус
                if (subMenuIndex == 0) {
                    dayPauseStartHour--;
                    if (dayPauseStartHour == -1) dayPauseStartHour = 25; // Переход в OFF
                    else if (dayPauseStartHour == 24) dayPauseStartHour = 23; // Возврат с OFF на 23
                }
                else if (subMenuIndex == 1) dayPauseStartMinute = (dayPauseStartMinute - 1 + 60) % 60;
                else if (subMenuIndex == 2) {
                    dayPauseEndHour--;
                    if (dayPauseEndHour == -1) dayPauseEndHour = 25;
                    else if (dayPauseEndHour == 24) dayPauseEndHour = 23;
                }
                else if (subMenuIndex == 3) dayPauseEndMinute = (dayPauseEndMinute - 1 + 60) % 60;
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK) {
                if (settingsChanged) saveLightTimers();
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;
            
        // --- ЭКРАН КОРМУШКИ ---
case SCREEN_FEEDER:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 6; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 6) % 6; needsRedraw = true; }
            else if (action == UP) { // Плюс
                if (subMenuIndex == 0) {
                    feed1Hour++;
                    if (feed1Hour == 24) feed1Hour = 25;
                    else if (feed1Hour > 25) feed1Hour = 0;
                }
                else if (subMenuIndex == 1) feed1Minute = (feed1Minute + 1) % 60;
                else if (subMenuIndex == 2) {
                    feed2Hour++;
                    if (feed2Hour == 24) feed2Hour = 25;
                    else if (feed2Hour > 25) feed2Hour = 0;
                }
                else if (subMenuIndex == 3) feed2Minute = (feed2Minute + 1) % 60;
                else if (subMenuIndex == 4) feedDuration += 100;
                else if (subMenuIndex == 5) feedFilterPauseEnabled = true;
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) { // Минус
                if (subMenuIndex == 0) {
                    feed1Hour--;
                    if (feed1Hour == -1) feed1Hour = 25;
                    else if (feed1Hour == 24) feed1Hour = 23;
                }
                else if (subMenuIndex == 1) feed1Minute = (feed1Minute - 1 + 60) % 60;
                else if (subMenuIndex == 2) {
                    feed2Hour--;
                    if (feed2Hour == -1) feed2Hour = 25;
                    else if (feed2Hour == 24) feed2Hour = 23;
                }
                else if (subMenuIndex == 3) feed2Minute = (feed2Minute - 1 + 60) % 60;
                else if (subMenuIndex == 4 && feedDuration > 100) feedDuration -= 100;
                else if (subMenuIndex == 5) feedFilterPauseEnabled = false;
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK) {
                if (settingsChanged) saveFeederSettings();
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;

        // --- ЭКРАН ДОЗАТОРОВ ---
        case SCREEN_DOSERS:
            if (action == RIGHT) { subMenuIndex = (subMenuIndex + 1) % 12; needsRedraw = true; }
            else if (action == LEFT) { subMenuIndex = (subMenuIndex - 1 + 12) % 12; needsRedraw = true; }
            else if (action == UP) { // Плюс
                if (subMenuIndex == 0) selectedChannel = (selectedChannel + 1) % 4;
                else if (subMenuIndex == 1) fertTriggerHour[selectedChannel] = (fertTriggerHour[selectedChannel] + 1) % 24;
                else if (subMenuIndex == 2) fertTriggerMinute[selectedChannel] = (fertTriggerMinute[selectedChannel] + 1) % 60;
                else if (subMenuIndex == 3) {
                    if (fertMotorRunTime[selectedChannel] < 1) fertMotorRunTime[selectedChannel] = 1;
                    else if (fertMotorRunTime[selectedChannel] < 300) fertMotorRunTime[selectedChannel]++;
                    else fertMotorRunTime[selectedChannel] = 300;
                }
                else if (subMenuIndex == 4) fertEnabled[selectedChannel] = !fertEnabled[selectedChannel];
                else fertDays[selectedChannel][subMenuIndex - 5] = !fertDays[selectedChannel][subMenuIndex - 5];
                settingsChanged = true; needsRedraw = true;
            } else if (action == DOWN) { // Минус
                if (subMenuIndex == 0) selectedChannel = (selectedChannel - 1 + 4) % 4;
                else if (subMenuIndex == 1) fertTriggerHour[selectedChannel] = (fertTriggerHour[selectedChannel] - 1 + 24) % 24;
                else if (subMenuIndex == 2) fertTriggerMinute[selectedChannel] = (fertTriggerMinute[selectedChannel] - 1 + 60) % 60;
                else if (subMenuIndex == 3) {
                    if (fertMotorRunTime[selectedChannel] > 300) fertMotorRunTime[selectedChannel] = 300;
                    else if (fertMotorRunTime[selectedChannel] > 1) fertMotorRunTime[selectedChannel]--;
                    else fertMotorRunTime[selectedChannel] = 1;
                }
                else if (subMenuIndex == 4) fertEnabled[selectedChannel] = !fertEnabled[selectedChannel];
                else fertDays[selectedChannel][subMenuIndex - 5] = !fertDays[selectedChannel][subMenuIndex - 5];
                settingsChanged = true; needsRedraw = true;
            } else if (action == CLICK) {
                if (settingsChanged) saveDoserSettings();
                settingsChanged = false; currentScreen = SCREEN_MENU_ROOT; lcd.clear();
            }
            break;
    }

    // --- ОБНОВЛЕНИЕ ЭКРАНА ---

    static ScreenState lastDrawnScreen = SCREEN_MAIN;
    static bool forceScreenRedraw = false;
    bool screenJustChanged = (currentScreen != lastDrawnScreen) || forceScreenRedraw;
    forceScreenRedraw = false;

      if (currentScreen == SCREEN_MAIN) {
        static int lastShownHour = -1, lastShownMinute = -1;
        static int lastShownBrightness[5] = {-1, -1, -1, -1, -1};
        static bool lastShownIsAP = true;
        static const char* lastShownMode = "";
        static int lastShownTempTenths = -1000;
        static int lastShownHeatsinkTenths = -1000; // Для отслеживания температуры LED

        bool isAP = (WiFi.getMode() == WIFI_AP);
        const char* modeLabel = getLightModeLabel();
        int tempTenths = (int)(tempWater * 10.0f); 
        int heatsinkTenths = (int)(tempHeatsink * 10.0f); // Считываем десятые доли LED

        bool mainDataChanged = (displayData.hour != lastShownHour) ||
                                (displayData.minute != lastShownMinute) ||
                                (isAP != lastShownIsAP) ||
                                (modeLabel != lastShownMode) ||
                                (tempTenths != lastShownTempTenths) ||
                                (heatsinkTenths != lastShownHeatsinkTenths); // Добавили в проверку
        for (int i = 0; i < 5 && !mainDataChanged; i++) {
            if (displayData.brightness[i] != lastShownBrightness[i]) mainDataChanged = true;
        }

        bool forceRefresh = (millis() - lastDisplayUpdate > 30000);
        bool didFullRedraw = false;

        if (mainDataChanged || needsRedraw || screenJustChanged || forceRefresh) {
            drawMainScreen();
            didFullRedraw = true;
            lastDisplayUpdate = millis();
            lastShownHour = displayData.hour;
            lastShownMinute = displayData.minute;
            lastShownIsAP = isAP;
            lastShownMode = modeLabel;
            lastShownTempTenths = tempTenths;
            lastShownHeatsinkTenths = heatsinkTenths; // Запоминаем значение LED
            for (int i = 0; i < 5; i++) lastShownBrightness[i] = displayData.brightness[i];

            // Рисуем звездочку поверх пробела, если вентилятор работает при перерисовке экрана
            if (isFanOn) {
                lcd.setCursor(19, 3);
                lcd.print("*");
            }
        }

        // Мигание двоеточия времени
        static unsigned long lastBlinkToggle = 0;
        if (!didFullRedraw && (millis() - lastBlinkToggle >= 500)) {
            lastBlinkToggle = millis();
            colonVisible = !colonVisible;
            lcd.setCursor(TIME_COLON_COL, 0);
            lcd.print(colonVisible ? ":" : " ");
        } else if (didFullRedraw) {
            lastBlinkToggle = millis();
        }

        // Точечное обновление значка вентилятора при включении/выключении
        static bool lastFanState = false;
        if (isFanOn && !lastFanState) {
            lcd.setCursor(19, 3);
            lcd.print("*"); // Вентилятор включился
        } else if (!isFanOn && lastFanState) {
            lcd.setCursor(19, 3);
            lcd.print(" "); // Вентилятор выключился
        }
        lastFanState = isFanOn;

    } else if (currentScreen == SCREEN_MAIN2) {
        // Тот же принцип диф-перерисовки для второго главного экрана (статус
        // реле + IP-адрес) - обновляем LCD только когда что-то реально
        // изменилось, плюс страховочный refresh раз в 30 секунд.
        static bool lastShownRelay[4] = {false, false, false, false};
        static int lastShownControlType[4] = {-1, -1, -1, -1};
        static IPAddress lastShownIP;
        static unsigned long lastMain2Refresh = 0;

        bool isAP = (WiFi.getMode() == WIFI_AP);
        IPAddress currentIP = isAP ? WiFi.softAPIP() : WiFi.localIP();

        bool relayDataChanged = (currentIP != lastShownIP);
        for (int i = 0; i < 4 && !relayDataChanged; i++) {
            if (relayState[i] != lastShownRelay[i] || controlType[i] != lastShownControlType[i]) {
                relayDataChanged = true;
            }
        }

        bool forceRefresh = (millis() - lastMain2Refresh > 30000);

        if (relayDataChanged || needsRedraw || screenJustChanged || forceRefresh) {
            drawRelayStatusScreen();
            lastMain2Refresh = millis();
            lastShownIP = currentIP;
            for (int i = 0; i < 4; i++) {
                lastShownRelay[i] = relayState[i];
                lastShownControlType[i] = controlType[i];
            }
        }
    } else if (needsRedraw || screenJustChanged) {
        if (currentScreen == SCREEN_MENU_ROOT) drawRootMenu();
        else if (currentScreen == SCREEN_LIGHT_PWM) drawLightPwmMenu();
        else if (currentScreen == SCREEN_TIME) drawTimeMenu();
        else if (currentScreen == SCREEN_RELAYS) drawRelaysMenu();
        else if (currentScreen == SCREEN_RELAY_THERM) drawRelayThermMenu();
        else if (currentScreen == SCREEN_RELAY_SCHED) drawRelaySchedMenu();
        else if (currentScreen == SCREEN_FAN_SET) drawFanMenu();
        else if (currentScreen == SCREEN_FEEDER) drawFeederMenu();
        else if (currentScreen == SCREEN_MOON_PWM) drawMoonPwmMenu();
        else if (currentScreen == SCREEN_LIGHT_SCHED) drawLightSchedMenu();
        else if (currentScreen == SCREEN_DAY_PAUSE) drawDayPauseMenu();
        else if (currentScreen == SCREEN_DOSERS) drawDosersMenu();
    }

    if (filterNotFoundWarningActive) {
        if (millis() - filterNotFoundWarningStartedAt < filterNotFoundWarningDurationMs) {
            lcd.setCursor(0, 0); lcd.print("FILTER NOT FOUND    ");
            lcd.setCursor(0, 1); lcd.print("Set outlet name:    ");
            lcd.setCursor(0, 2); lcd.print("'Filter' then retry ");
            lcd.setCursor(0, 3); lcd.print("Message clears in 5s");
        } else {
            filterNotFoundWarningActive = false;
            lcd.clear();
            forceScreenRedraw = true;
        }
    }

    lastDrawnScreen = currentScreen;
}
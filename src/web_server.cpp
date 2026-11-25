#include "web_server.h"
#include "espnow_sync.h"
#include <WebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ElegantOTA.h>
#include <sys/time.h> // Для аппаратной установки времени ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Подключаем наши модули
#include "memory.h"
#include "doser.h"
#include "feeder.h"
#include "lighting.h"
#include "relays.h"
#include "fan.h"
#include "temperature.h"
#include "mqtt_service.h"

// Доступ к текущему времени из main.cpp
extern int currentHour;
extern int currentMinute;
extern String relayNames[4];
extern String doserNames[4];

extern Preferences preferences;
extern String ssid;
extern String password;
extern long gmtOffset_sec;

// Доступ к переменным таймеров света
extern int lightOnHour;
extern int lightOnMinute;
extern int lightOffHour;
extern int lightOffMinute;
extern int sunriseDuration;
extern int dayPauseStartHour;
extern int dayPauseStartMinute;
extern int dayPauseEndHour;
extern int dayPauseEndMinute;
extern bool isMoonlightEnabled;

WebServer server(80);

namespace {
bool processingMqttRequest = false;
JsonObjectConst mqttRequestArgs;
int mqttResponseStatus = 500;
String mqttResponseType;
String mqttResponseBody;

bool apiHasArg(const String& name) {
    return processingMqttRequest
        ? mqttRequestArgs[name.c_str()].is<const char*>()
        : server.hasArg(name);
}

String apiArg(const String& name) {
    if (!processingMqttRequest) return server.arg(name);
    JsonVariantConst value = mqttRequestArgs[name];
    if (value.is<const char*>()) return String(value.as<const char*>());
    char buffer[32];
    serializeJson(value, buffer, sizeof(buffer));
    return String(buffer);
}

void apiSend(int status, const String& type, const String& body) {
    if (processingMqttRequest) {
        mqttResponseStatus = status;
        mqttResponseType = type;
        mqttResponseBody = body;
    } else {
        server.send(status, type, body);
    }
}

void apiSend(int status) {
    apiSend(status, "text/plain", "");
}
}

enum MonitorOtaState : uint8_t {
    MONITOR_OTA_IDLE,
    MONITOR_OTA_TRANSFER,
    MONITOR_OTA_COMPLETE,
    MONITOR_OTA_FAILED
};

static volatile uint8_t monitorOtaState = MONITOR_OTA_IDLE;
static volatile uint32_t monitorOtaSent = 0;
static volatile uint32_t monitorOtaTotal = 0;
static volatile uint8_t monitorOtaError = 0;
static volatile bool monitorFirmwareUploadOk = false;
static volatile uint32_t monitorFirmwareUploadBytes = 0;

// ==============================================================================
// 1. MAIN PAGE (index_html) - ENGLISH
// ==============================================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SiliconForge AquaOS</title>
    <style>
        :root { --bg: #121212; --card: #1e1e1e; --text: #e0e0e0; --accent: #00bcd4; --danger: #f44336; }
        body { font-family: 'Segoe UI', Tahoma, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; }
        h1 { text-align: center; color: var(--accent); text-transform: uppercase; letter-spacing: 2px; margin-bottom: 30px; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 20px; max-width: 1200px; margin: 0 auto; }
        .card { background: var(--card); padding: 20px; border-radius: 12px; box-shadow: 0 4px 10px rgba(0,0,0,0.5); border-top: 4px solid var(--accent); }
        h2 { margin-top: 0; font-size: 1.3rem; border-bottom: 1px solid #333; padding-bottom: 10px; }
        .row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; }
        input[type="number"], input[type="time"], input[type="text"], input[type="password"], select { background: #333; color: #fff; border: 1px solid #555; padding: 8px; border-radius: 4px; width: 110px; text-align: center; }
        .name-input { width: 140px !important; text-align: left !important; }
        .mqtt-field { display: flex; flex-direction: column; gap: 6px; margin-bottom: 12px; }
        .mqtt-field input, .mqtt-field textarea { background: #333; color: #fff; border: 1px solid #555; padding: 8px; border-radius: 4px; width: 100%; box-sizing: border-box; text-align: left; }
        .mqtt-field textarea { min-height: 120px; resize: vertical; font-family: monospace; }
        select { width: 100%; margin-bottom: 15px; text-align: left; }
        input[type="range"] { width: 100%; margin-top: 5px; }
        button, .btn-link { display: block; text-align: center; background: var(--accent); color: #000; border: none; padding: 10px; border-radius: 4px; cursor: pointer; font-weight: bold; width: 100%; margin-top: 10px; text-decoration: none; box-sizing: border-box; }
        button:hover, .btn-link:hover { background: #0097a7; }
        .btn-manual { background: #ff9800; margin-top: 5px; }
        .btn-sync { background: #4CAF50; color: white; margin-top: 5px; }
        .btn-sync:hover { background: #388E3C; }
        .btn-sched { background: #4CAF50; color: white; margin-top: 15px; }
        .badge { background: #333; padding: 5px 10px; border-radius: 12px; font-size: 0.9rem; }
        .toast { visibility: hidden; min-width: 250px; background-color: #4CAF50; color: white; text-align: center; border-radius: 4px; padding: 16px; position: fixed; z-index: 1; left: 50%; bottom: 30px; transform: translateX(-50%); }
        .toast.show { visibility: visible; animation: fadein 0.5s, fadeout 0.5s 2.5s; }
        @keyframes fadein { from {bottom: 0; opacity: 0;} to {bottom: 30px; opacity: 1;} }
        @keyframes fadeout { from {bottom: 30px; opacity: 1;} to {bottom: 0; opacity: 0;} }
    </style>
</head>
<body>
    <h1>SiliconForge AquaOS</h1>
    
    <div class="grid">

        <!-- 1. STATUS -->
        <div class="card">
            <h2>📊 Current Status</h2>
            <div class="row"><span>Water:</span> <span class="badge" id="tW">-- °C</span></div>
            <div class="row"><span>Heatsink:</span> <span class="badge" id="tH">-- °C</span></div>
            <div class="row"><span>Cooling Fan:</span> <span class="badge" id="fanState">--</span></div>
        </div>

        <!-- 2. LIGHTING (MAIN) -->
        <div class="card">
            <h2>💡 Main Light (0-100%)</h2>
            <div class="row" style="margin-bottom: 5px;"><span>RED:</span> <span id="val0">0%</span></div>
            <input type="range" id="ch0" min="0" max="100" onchange="setLight(0, this.value)" oninput="document.getElementById('val0').innerText=this.value+'%'">
            
            <div class="row" style="margin-bottom: 5px; margin-top: 15px;"><span>BLUE:</span> <span id="val1">0%</span></div>
            <input type="range" id="ch1" min="0" max="100" onchange="setLight(1, this.value)" oninput="document.getElementById('val1').innerText=this.value+'%'">
            
            <div class="row" style="margin-bottom: 5px; margin-top: 15px;"><span>WHITE 1:</span> <span id="val2">0%</span></div>
            <input type="range" id="ch2" min="0" max="100" onchange="setLight(2, this.value)" oninput="document.getElementById('val2').innerText=this.value+'%'">
            
            <div class="row" style="margin-bottom: 5px; margin-top: 15px;"><span>WHITE 2:</span> <span id="val3">0%</span></div>
            <input type="range" id="ch3" min="0" max="100" onchange="setLight(3, this.value)" oninput="document.getElementById('val3').innerText=this.value+'%'">
            
            <div class="row" style="margin-bottom: 5px; margin-top: 15px;"><span>WHITE 3:</span> <span id="val4">0%</span></div>
            <input type="range" id="ch4" min="0" max="100" onchange="setLight(4, this.value)" oninput="document.getElementById('val4').innerText=this.value+'%'">
        </div>

        <!-- 3. LIGHTING (MOON) -->
        <div class="card">
            <h2>🌙 Moonlight (0-100%)</h2>
            <div class="row" style="margin-bottom: 5px;"><span>RED:</span> <span id="mval0">0%</span></div>
            <input type="range" id="m0" min="0" max="100" onchange="setMoon(0, this.value)" oninput="document.getElementById('mval0').innerText=this.value+'%'">
            
            <div class="row" style="margin-bottom: 5px; margin-top: 15px;"><span>BLUE:</span> <span id="mval1">0%</span></div>
            <input type="range" id="m1" min="0" max="100" onchange="setMoon(1, this.value)" oninput="document.getElementById('mval1').innerText=this.value+'%'">
            
            <div class="row" style="margin-bottom: 5px; margin-top: 15px;"><span>WHITE 1:</span> <span id="mval2">0%</span></div>
            <input type="range" id="m2" min="0" max="100" onchange="setMoon(2, this.value)" oninput="document.getElementById('mval2').innerText=this.value+'%'">
            
            <div class="row" style="margin-bottom: 5px; margin-top: 15px;"><span>WHITE 2:</span> <span id="mval3">0%</span></div>
            <input type="range" id="m3" min="0" max="100" onchange="setMoon(3, this.value)" oninput="document.getElementById('mval3').innerText=this.value+'%'">
            
            <div class="row" style="margin-bottom: 5px; margin-top: 15px;"><span>WHITE 3:</span> <span id="mval4">0%</span></div>
            <input type="range" id="m4" min="0" max="100" onchange="setMoon(4, this.value)" oninput="document.getElementById('mval4').innerText=this.value+'%'">
        </div>

        <!-- 4. LIGHT TIMERS & PAUSE -->
        <div class="card">
            <h2>🕒 Light Timers & Pause</h2>
            <div class="row"><label>Light ON:</label> <input type="time" id="lOn"></div>
            <div class="row"><label>Light OFF:</label> <input type="time" id="lOff"></div>
            <div class="row"><label>Fade (min):</label> <input type="number" id="lFade" min="0" step="1"></div>
            <div class="row"><label>Moonlight:</label> 
                <select id="mMd" style="width:110px; margin-bottom:0;">
                    <option value="1">Enabled</option>
                    <option value="0">Disabled</option>
                </select>
            </div>
<hr style="border-color: #333; margin: 15px 0;">
            <div class="row">
                <label style="display:flex; align-items:center; gap:5px;">
                    <input type="checkbox" id="pEnCb" onchange="togglePause()"> Enable Pause
                </label>
            </div>
            <div class="row"><label>Pause Start:</label> <input type="time" id="pStart"></div>
            <div class="row"><label>Pause End:</label> <input type="time" id="pEnd"></div>
            <button onclick="saveLightTimers()">Save Light Settings</button>
        </div>

        <!-- 5. AUTO FEEDER -->
            <div class="card">
            <h2>🐟 Auto Feeder</h2>
            <div class="row">
                <label style="display:flex; align-items:center; gap:5px;">
                    <input type="checkbox" id="f1EnCb" onchange="document.getElementById('feed1').disabled = !this.checked"> Portion 1:
                </label> 
                <input type="time" id="feed1">
            </div>
            <div class="row">
                <label style="display:flex; align-items:center; gap:5px;">
                    <input type="checkbox" id="f2EnCb" onchange="document.getElementById('feed2').disabled = !this.checked"> Portion 2:
                </label> 
                <input type="time" id="feed2">
            </div>
            <div class="row"><label>Duration (s):</label> <input type="number" id="feedDur" step="0.1" min="0.1"></div>
            <div class="row">
                <label style="display:flex; align-items:center; gap:5px;">
                    <input type="checkbox" id="feedFilterPause"> Pause Filter for 10 minutes
                </label>
            </div>
            <button onclick="saveFeeder()">Save Timers</button>
            <button class="btn-manual" onclick="manualAction('/api/manual_feed')">Feed Now</button>
        </div>

        <!-- 6. COOLING FAN -->
        <div class="card">
            <h2>❄️ LED Cooling Fan</h2>
            <div class="row"><label>Turn ON (°C):</label> <input type="number" id="fanOn" step="0.5"></div>
            <div class="row"><label>Max Speed (°C):</label> <input type="number" id="fanMax" step="0.5"></div>
            <div class="row"><label>Start Speed (%):</label> <input type="number" id="fanStart" step="1" min="0" max="100"></div>
            <div class="row"><label>Hysteresis (°C):</label> <input type="number" id="fanHys" step="0.5"></div>
            <button onclick="saveFan()">Save Fan Settings</button>
        </div>

        <!-- 7. RELAYS -->
        <div class="card">
            <h2>🔌 Relays (Sockets)</h2>
            <select id="relSel" onchange="updateRelayUI()"></select>
            <div class="row"><label>Name:</label> <input type="text" id="relName" class="name-input" maxlength="15"></div>
            <div class="row"><label>Mode:</label> 
                <select id="relMode" style="width:110px; margin-bottom:0;">
                    <option value="0">Thermostat</option>
                    <option value="1">Timer</option>
                </select>
            </div>
            <div class="row"><label>Target t (°C):</label> <input type="number" id="relSp" step="0.1"></div>
            <div class="row"><label>Hysteresis (°C):</label> <input type="number" id="relHys" step="0.1"></div>
            <button onclick="saveRelay()">Save Relay Settings</button>
            <a href="/schedule" class="btn-link btn-sched">📅 Schedule Setup (24h)</a>
        </div>

        <!-- 8. DOSING PUMPS -->
        <div class="card">
            <h2>🧪 Dosing Pumps (Fertilizers)</h2>
            <select id="dosSel" onchange="updateDoserUI()"></select>
            <div class="row"><label>Name:</label> <input type="text" id="dosName" class="name-input" maxlength="15"></div>
            <div class="row"><label>Time (HH:MM):</label> <input type="time" id="dosTime"></div>
            <div class="row"><label>Run time (sec):</label> <input type="number" id="dosRun" min="0.1" max="30" step="0.1" required></div>
            <div class="row"><label>Fertilizer dosing:</label> <label><input type="checkbox" id="dosEnabled"> Enabled</label></div>
            <div class="row"><label>Days:</label><div style="display:flex;gap:6px;flex-wrap:wrap">
                <label>Su<input type="checkbox" id="dosDay0"></label>
                <label>Mo<input type="checkbox" id="dosDay1"></label>
                <label>Tu<input type="checkbox" id="dosDay2"></label>
                <label>We<input type="checkbox" id="dosDay3"></label>
                <label>Th<input type="checkbox" id="dosDay4"></label>
                <label>Fr<input type="checkbox" id="dosDay5"></label>
                <label>Sa<input type="checkbox" id="dosDay6"></label>
            </div></div>
            <button onclick="saveDoser()">Save Doser Settings</button>
        </div>

        <!-- 9. SYSTEM & NETWORK -->
        <div class="card">
            <h2>⚙️ System & Network</h2>
            <div class="row"><label>WiFi SSID:</label> <input type="text" id="sysSsid"></div>
            <div class="row"><label>WiFi Password:</label> <input type="password" id="sysPass" placeholder="New Password"></div>
            <div class="row"><label>Timezone (GMT):</label> <input type="number" id="sysGmt" min="-12" max="14"></div>
            <button onclick="saveSystem()">Save & Reboot</button>
            
            <hr style="border-color: #333; margin: 20px 0 15px 0;">
            <div class="row"><label>Device Time:</label> <input type="time" id="sysTime"></div>
            <button onclick="saveManualTime()">Set Time Manually</button>
            <button class="btn-sync" onclick="syncBrowserTime()">📱 Sync with Smartphone</button>
            
        </div>

        <div class="card">
            <h2>🚀 Firmware Updates</h2>
            <a href="/update" class="btn-link" style="background: #E91E63; color: white;">Main Firmware Update</a>

            <form action="/api/monitor_ota_upload" method="POST" enctype="multipart/form-data" style="margin-top: 12px;">
                <input id="monitorFirmware" type="file" name="firmware" accept=".bin" style="display: none;" onchange="uploadMonitorFirmware(this.form)">
                <button type="button" onclick="document.getElementById('monitorFirmware').click()" style="background: #8a2be2; color: white;">📦 Upload Monitor Firmware</button>
            </form>

            <form id="monitorOtaSend" action="/api/monitor_ota" method="POST" style="margin-top: 10px;" onsubmit="startMonitorOta(event)">
                <button type="submit" style="background: #007bff; color: white;">📡 Send Firmware to Monitor</button>
            </form>
            <progress id="monitorOtaProgress" value="0" max="100" style="display: none; width: 100%; margin-top: 12px; height: 18px;"></progress>
            <div id="monitorOtaStatus" role="status" aria-live="polite" style="margin-top: 8px;"></div>
        </div>

        <div class="card">
            <h2>📡 MQTT Service</h2>
            <div class="row"><label for="mqttEnabled">Enable MQTT:</label><input type="checkbox" id="mqttEnabled"></div>
            <div class="row"><span>Connection:</span><span class="badge" id="mqttStatus">--</span></div>
            <div id="mqttDetail" role="status" style="margin-bottom: 12px;"></div>
            <div class="mqtt-field"><label for="mqttHost">Broker host:</label><input type="text" id="mqttHost" autocomplete="off"></div>
            <div class="mqtt-field"><label for="mqttPort">Broker port:</label><input type="number" id="mqttPort" min="1" max="65535"></div>
            <div class="mqtt-field"><label for="mqttUsername">Username:</label><input type="text" id="mqttUsername" autocomplete="off"></div>
            <div class="mqtt-field"><label for="mqttPassword">Password (leave blank to keep current):</label><input type="password" id="mqttPassword" autocomplete="new-password"></div>
            <div class="row"><label for="mqttClearPassword">Clear stored password:</label><input type="checkbox" id="mqttClearPassword"></div>
            <div class="row"><label for="mqttTls">Use TLS:</label><input type="checkbox" id="mqttTls"></div>
            <div class="mqtt-field"><label for="mqttCa">Broker CA certificate (PEM):</label><textarea id="mqttCa" spellcheck="false"></textarea></div>
            <div class="mqtt-field"><label for="mqttCustomDeviceId">Device ID (letters, digits, - or _; blank uses MAC):</label><input type="text" id="mqttCustomDeviceId" maxlength="32" pattern="[A-Za-z0-9_-]{1,32}" autocomplete="off"></div>
            <div class="row"><span>Active Device ID:</span><span class="badge" id="mqttDeviceId">--</span></div>
            <button onclick="saveMqttSettings()">Save MQTT Settings</button>
        </div>

    </div>

    <div id="toast" class="toast">Saved!</div>

    <script>
        let globalData = {};
        let currentStatusRefreshInProgress = false;
        let monitorOtaPollTimer = null;

        function refreshCurrentStatus() {
            if (currentStatusRefreshInProgress) return;
            currentStatusRefreshInProgress = true;

            fetch('/api/state', { cache: 'no-store' })
                .then(response => {
                    if (!response.ok) throw new Error('Status request failed');
                    return response.json();
                })
                .then(data => {
                    document.getElementById('tW').innerText = data.tW + ' °C';
                    document.getElementById('tH').innerText = data.tH + ' °C';
                    document.getElementById('fanState').innerText = data.fan ? 'ON' : 'OFF';
                    updateMqttStatus(data);
                })
                .catch(() => {})
                .then(() => { currentStatusRefreshInProgress = false; });
        }

        function updateMonitorOtaProgress(value, message) {
            const progress = document.getElementById('monitorOtaProgress');
            progress.style.display = 'block';
            progress.value = Math.max(0, Math.min(100, value));
            document.getElementById('monitorOtaStatus').innerText = message;
        }

        function uploadMonitorFirmware(form) {
            const fileInput = form.querySelector('input[type="file"]');
            if (!fileInput.files.length) return;

            const request = new XMLHttpRequest();
            request.upload.addEventListener('progress', event => {
                if (event.lengthComputable) {
                    const percent = Math.round(event.loaded * 100 / event.total);
                    updateMonitorOtaProgress(percent, `Uploading a file to ESP32: ${percent}%`);
                }
            });
            request.onload = () => {
                if (request.status >= 200 && request.status < 300) {
                    updateMonitorOtaProgress(100, 'File saved to ESP32. You can now send it to the monitor.');
                } else {
                    updateMonitorOtaProgress(0, `Error uploading file: ${request.responseText || request.status}`);
                }
            };
            request.onerror = () => updateMonitorOtaProgress(0, 'Error connecting while uploading file to ESP32.');
            request.open('POST', form.action);
            request.send(new FormData(form));
        }

        async function pollMonitorOtaStatus() {
            try {
                const response = await fetch('/api/monitor_ota_status', { cache: 'no-store' });
                const status = await response.json();
                const sendButton = document.querySelector('#monitorOtaSend button');

                if (status.state === 1) {
                    const percent = status.total ? Math.floor(status.sent * 100 / status.total) : 0;
                    updateMonitorOtaProgress(percent, `Transferring to monitor: ${percent}% (${status.sent}/${status.total} bytes confirmed)`);
                    monitorOtaPollTimer = setTimeout(pollMonitorOtaStatus, 400);
                } else if (status.state === 2) {
                    updateMonitorOtaProgress(100, 'Monitor confirmed firmware update. Restarting...');
                    sendButton.disabled = false;
                } else if (status.state === 3) {
                    const errors = ['', 'Failed to open firmware file.', 'Firmware file is empty.', 'Monitor did not confirm update start.', 'Failed to read firmware file.', 'Monitor did not confirm data block.', 'Monitor did not confirm update completion.'];
                    updateMonitorOtaProgress(0, `Update error: ${errors[status.error] || 'unknown error'}`);
                    sendButton.disabled = false;
                }
            } catch (error) {
                monitorOtaPollTimer = setTimeout(pollMonitorOtaStatus, 1000);
            }
        }

        async function startMonitorOta(event) {
            event.preventDefault();
            const sendButton = event.currentTarget.querySelector('button');
            sendButton.disabled = true;
            updateMonitorOtaProgress(0, 'Starting transfer to monitor...');
            try {
                const response = await fetch(event.currentTarget.action, { method: 'POST' });
                if (!response.ok) throw new Error(await response.text());
                if (monitorOtaPollTimer) clearTimeout(monitorOtaPollTimer);
                pollMonitorOtaStatus();
            } catch (error) {
                updateMonitorOtaProgress(0, `Failed to start update: ${error.message}`);
                sendButton.disabled = false;
            }
        }

        function togglePause() {
            let en = document.getElementById('pEnCb').checked;
            document.getElementById('pStart').disabled = !en;
            document.getElementById('pEnd').disabled = !en;
        }
        
        function showToast(msg) {
            let el = document.getElementById("toast");
            el.innerText = msg;
            el.className = "toast show";
            setTimeout(() => { el.className = el.className.replace("toast show", "toast"); }, 3000);
        }
        
        function fmtTime(h, m) { return (h < 10 ? '0' : '') + h + ':' + (m < 10 ? '0' : '') + m; }

        function populateSelect(selId, namesArray) {
            let sel = document.getElementById(selId);
            let currentVal = sel.value || 0;
            sel.innerHTML = '';
            for(let i=0; i<namesArray.length; i++) {
                let opt = document.createElement('option');
                opt.value = i;
                opt.text = namesArray[i];
                sel.appendChild(opt);
            }
            sel.value = currentVal;
        }

        window.onload = function() {
            setInterval(refreshCurrentStatus, 1000);
            fetch('/api/state').then(r => r.json()).then(data => {
                globalData = data;
                document.getElementById('sysSsid').value = data.ssid;
                document.getElementById('sysGmt').value = data.gmt;
                document.getElementById('sysTime').value = fmtTime(data.cH, data.cM); 
                updateMqttStatus(data);

                document.getElementById('tW').innerText = data.tW + ' °C';
                document.getElementById('tH').innerText = data.tH + ' °C';
                document.getElementById('fanState').innerText = data.fan ? "ON" : "OFF";
                
                for(let i=0; i<5; i++) {
                    document.getElementById('ch'+i).value = data.l[i];
                    document.getElementById('val'+i).innerText = data.l[i] + '%';
                    document.getElementById('m'+i).value = data.m[i];
                    document.getElementById('mval'+i).innerText = data.m[i] + '%';
                }
                
                // Light Timers & Pause Data
                document.getElementById('lOn').value = fmtTime(data.lOnH, data.lOnM);
                document.getElementById('lOff').value = fmtTime(data.lOffH, data.lOffM);
                document.getElementById('lFade').value = data.fade;
                document.getElementById('mMd').value = data.moonEn ? "1" : "0";
                
                // --- НОВЫЙ КОД ПАУЗЫ ---
                let pEn = (data.pStH != 25);
                document.getElementById('pEnCb').checked = pEn;
                togglePause();
                document.getElementById('pStart').value = pEn ? fmtTime(data.pStH, data.pStM) : "";
                document.getElementById('pEnd').value = (data.pEnH != 25) ? fmtTime(data.pEnH, data.pEnM) : "";
                // -----------------------

                document.getElementById('fanOn').value = data.fOn;
                document.getElementById('fanMax').value = data.fMax;
                document.getElementById('fanStart').value = data.fStart;
                document.getElementById('fanHys').value = data.fHys;
                
                // --- НОВЫЙ КОД КОРМУШКИ ---
                let f1En = (data.fd1H != 25);
                document.getElementById('f1EnCb').checked = f1En;
                document.getElementById('feed1').disabled = !f1En;
                document.getElementById('feed1').value = f1En ? fmtTime(data.fd1H, data.fd1M) : "";
                
                let f2En = (data.fd2H != 25);
                document.getElementById('f2EnCb').checked = f2En;
                document.getElementById('feed2').disabled = !f2En;
                document.getElementById('feed2').value = f2En ? fmtTime(data.fd2H, data.fd2M) : "";
                document.getElementById('feedDur').value = (Number(data.fdDur) / 1000).toFixed(1);
                document.getElementById('feedFilterPause').checked = data.fdPause;
                // --------------------------

                populateSelect('relSel', globalData.rName);
                populateSelect('dosSel', globalData.dName);

                updateRelayUI();
                updateDoserUI();
            });
            fetch('/api/mqtt', { cache: 'no-store' }).then(response => {
                if (!response.ok) throw new Error('MQTT settings request failed');
                return response.json();
            }).then(data => {
                document.getElementById('mqttEnabled').checked = data.enabled;
                document.getElementById('mqttHost').value = data.host;
                document.getElementById('mqttPort').value = data.port;
                document.getElementById('mqttUsername').value = data.username;
                document.getElementById('mqttTls').checked = data.tls;
                document.getElementById('mqttCa').value = data.ca;
                document.getElementById('mqttCustomDeviceId').value = data.customDeviceId;
                document.getElementById('mqttDeviceId').innerText = data.deviceId;
            }).catch(error => showToast(error.message));
        };

        function updateRelayUI() {
            let i = document.getElementById('relSel').value;
            if(!globalData.rCtrl) return;
            document.getElementById('relName').value = globalData.rName[i];
            document.getElementById('relMode').value = globalData.rCtrl[i];
            document.getElementById('relSp').value = globalData.rSP[i];
            document.getElementById('relHys').value = globalData.rHys[i];
        }

        function updateDoserUI() {
            let i = document.getElementById('dosSel').value;
            if(!globalData.dHr) return;
            document.getElementById('dosName').value = globalData.dName[i];
            document.getElementById('dosTime').value = fmtTime(globalData.dHr[i], globalData.dMin[i]);
            document.getElementById('dosRun').value = (Math.max(1, Math.min(Number(globalData.dRun[i]) || 1, 300)) / 10).toFixed(1);
            document.getElementById('dosEnabled').checked = Boolean(globalData.dEnabled[i]);
            for (let day = 0; day < 7; day++) {
                document.getElementById('dosDay' + day).checked = Boolean(globalData.dDays[i][day]);
            }
        }

        function saveSystem() {
            let p = new URLSearchParams();
            p.append('ssid', document.getElementById('sysSsid').value);
            p.append('pass', document.getElementById('sysPass').value);
            p.append('gmt', document.getElementById('sysGmt').value);
            fetch('/api/set_sys', { method: 'POST', body: p }).then(() => {
                showToast("Rebooting...");
                setTimeout(() => location.reload(), 5000);
            });
        }

        function updateMqttStatus(data) {
            document.getElementById('mqttStatus').innerText = !data.mqttEnabled
                ? 'Disabled'
                : (data.mqttConnected ? 'Connected' : 'Disconnected');
            document.getElementById('mqttDetail').innerText = data.mqttDetail || '';
        }

        async function saveMqttSettings() {
            const p = new URLSearchParams();
            p.append('enabled', document.getElementById('mqttEnabled').checked ? '1' : '0');
            p.append('host', document.getElementById('mqttHost').value);
            p.append('port', document.getElementById('mqttPort').value);
            p.append('username', document.getElementById('mqttUsername').value);
            p.append('deviceId', document.getElementById('mqttCustomDeviceId').value);
            p.append('tls', document.getElementById('mqttTls').checked ? '1' : '0');
            p.append('ca', document.getElementById('mqttCa').value);
            const passwordInput = document.getElementById('mqttPassword');
            if (document.getElementById('mqttClearPassword').checked) {
                p.append('clearPassword', '1');
            } else if (passwordInput.value) {
                p.append('password', passwordInput.value);
            }

            try {
                const response = await fetch('/api/set_mqtt', { method: 'POST', body: p });
                if (!response.ok) throw new Error(await response.text());
                passwordInput.value = '';
                document.getElementById('mqttClearPassword').checked = false;
                try {
                    const settingsResponse = await fetch('/api/mqtt', { cache: 'no-store' });
                    if (!settingsResponse.ok) throw new Error('Settings reload request failed');
                    const settings = await settingsResponse.json();
                    document.getElementById('mqttCustomDeviceId').value = settings.customDeviceId;
                    document.getElementById('mqttDeviceId').innerText = settings.deviceId;
                } catch (error) {
                    showToast(`MQTT settings saved, but Device ID could not be refreshed: ${error.message}`);
                    return;
                }
                showToast('MQTT settings saved!');
            } catch (error) {
                showToast(`MQTT save failed: ${error.message}`);
            }
        }

        function saveManualTime() {
            let t = document.getElementById('sysTime').value.split(':');
            if(t.length < 2) return;
            let p = new URLSearchParams();
            p.append('h', t[0]); p.append('m', t[1]);
            fetch('/api/set_time', { method: 'POST', body: p }).then(() => showToast("Time Updated!"));
        }

        function syncBrowserTime() {
            let d = new Date();
            let p = new URLSearchParams();
            p.append('h', d.getHours());
            p.append('m', d.getMinutes());
            p.append('s', d.getSeconds());
            fetch('/api/set_time', { method: 'POST', body: p }).then(() => {
                document.getElementById('sysTime').value = fmtTime(d.getHours(), d.getMinutes());
                showToast("Synced with Smartphone!");
            });
        }

        function setLight(ch, val) { fetch(`/api/set_light?ch=${ch}&val=${val}`, { method: 'POST' }); }
        function setMoon(ch, val) { fetch(`/api/set_moon?ch=${ch}&val=${val}`, { method: 'POST' }); }

        function saveLightTimers() {
            let on = document.getElementById('lOn').value.split(':');
            let off = document.getElementById('lOff').value.split(':');
            let pSt = document.getElementById('pStart').value.split(':');
            let pEndArr = document.getElementById('pEnd').value.split(':'); // Изменено имя переменной
            
            let p = new URLSearchParams();
            p.append('onH', on[0] ? on[0] : 0); p.append('onM', on[1] ? on[1] : 0);
            p.append('offH', off[0] ? off[0] : 0); p.append('offM', off[1] ? off[1] : 0);
            p.append('fade', document.getElementById('lFade').value);
            p.append('mEn', document.getElementById('mMd').value);

            // --- НОВЫЙ КОД ПАУЗЫ ---
            let pEn = document.getElementById('pEnCb').checked; // Читаем состояние галочки
            p.append('pStH', pEn ? (pSt[0] ? pSt[0] : 0) : 25); // Если галочка стоит - шлем часы, если нет - 25
            p.append('pStM', pEn ? (pSt[1] ? pSt[1] : 0) : 0);
            p.append('pEnH', pEn ? (pEndArr[0] ? pEndArr[0] : 0) : 25);
            p.append('pEnM', pEn ? (pEndArr[1] ? pEndArr[1] : 0) : 0);
            // -----------------------
            
            fetch('/api/set_light_timers', { method: 'POST', body: p }).then(() => showToast("Timers Saved!"));
        }

        function saveRelay() {
            let i = document.getElementById('relSel').value;
            let p = new URLSearchParams();
            p.append('ch', i);
            p.append('name', document.getElementById('relName').value);
            p.append('mode', document.getElementById('relMode').value);
            p.append('sp', document.getElementById('relSp').value);
            p.append('hys', document.getElementById('relHys').value);
            
            globalData.rName[i] = document.getElementById('relName').value;
            globalData.rCtrl[i] = document.getElementById('relMode').value;
            globalData.rSP[i] = document.getElementById('relSp').value;
            globalData.rHys[i] = document.getElementById('relHys').value;
            
            populateSelect('relSel', globalData.rName);
            fetch('/api/set_relay', { method: 'POST', body: p }).then(() => showToast("Relay Settings Saved!"));
        }

        function saveDoser() {
            let i = document.getElementById('dosSel').value;
            let runInput = document.getElementById('dosRun');
            if (!runInput.reportValidity()) return;
            let t = document.getElementById('dosTime').value.split(':');
            let hour = Number(t[0]);
            let minute = Number(t[1]);
            if (t.length !== 2 || !Number.isInteger(hour) || hour < 0 || hour > 23 ||
                !Number.isInteger(minute) || minute < 0 || minute > 59) {
                showToast("Set a valid dosing time");
                return;
            }
            let p = new URLSearchParams();
            p.append('ch', i);
            p.append('name', document.getElementById('dosName').value);
            p.append('h', String(hour));
            p.append('m', String(minute));
            p.append('run', runInput.value);
            p.append('enabled', document.getElementById('dosEnabled').checked ? '1' : '0');
            for (let day = 0; day < 7; day++) {
                let enabled = document.getElementById('dosDay' + day).checked;
                p.append('day' + day, enabled ? '1' : '0');
            }

            fetch('/api/set_doser', { method: 'POST', body: p }).then(response => {
                if (!response.ok) throw new Error("Doser settings request failed");
                return fetch('/api/state', { cache: 'no-store' });
            }).then(response => {
                if (!response.ok) throw new Error("Failed to reload doser settings");
                return response.json();
            }).then(data => {
                globalData = data;
                let doserSelect = document.getElementById('dosSel');
                let selectedChannel = doserSelect.value;
                populateSelect('dosSel', globalData.dName);
                doserSelect.value = selectedChannel;
                updateDoserUI();
                showToast("Doser Settings Saved!");
            }).catch(error => {
                console.error(error);
                showToast("Failed to save doser settings");
            });
        }

        function saveFan() {
            let p = new URLSearchParams();
            p.append('on', document.getElementById('fanOn').value);
            p.append('max', document.getElementById('fanMax').value);
            p.append('start', document.getElementById('fanStart').value);
            p.append('hys', document.getElementById('fanHys').value);
            fetch('/api/set_fan', { method: 'POST', body: p }).then(() => showToast("Fan Settings Saved!"));
        }

        function saveFeeder() {
            let f1 = document.getElementById('feed1').value.split(':');
            let f2 = document.getElementById('feed2').value.split(':');
            let p = new URLSearchParams();
            
            // --- НОВЫЙ КОД КОРМУШКИ ---
            let f1En = document.getElementById('f1EnCb').checked;
            let f2En = document.getElementById('f2EnCb').checked;
            p.append('f1H', f1En ? (f1[0] ? f1[0] : 0) : 25);
            p.append('f1M', f1En ? (f1[1] ? f1[1] : 0) : 0);
            p.append('f2H', f2En ? (f2[0] ? f2[0] : 0) : 25);
            p.append('f2M', f2En ? (f2[1] ? f2[1] : 0) : 0);
            // --------------------------
            
            let durMs = Math.round(parseFloat(document.getElementById('feedDur').value) * 1000);
            p.append('dur', durMs);
            p.append('filterPause', document.getElementById('feedFilterPause').checked ? '1' : '0');
            
            fetch('/api/set_feeder', { method: 'POST', body: p }).then(() => showToast("Feeder Settings Saved!"));
        }

        function manualAction(url) { fetch(url, { method: 'POST' }).then(() => showToast("Command Executed!")); }
    </script>
</body>
</html>
)rawliteral";

// ==============================================================================
// 2. SCHEDULE PAGE (schedule_html) - ENGLISH
// ==============================================================================
const char schedule_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Relay Schedule</title>
    <style>
        :root { --bg: #121212; --card: #1e1e1e; --text: #e0e0e0; --accent: #00bcd4; }
        body { font-family: 'Segoe UI', Tahoma, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; }
        h1 { text-align: center; color: var(--accent); }
        .card { background: var(--card); padding: 20px; border-radius: 12px; max-width: 800px; margin: 0 auto; box-shadow: 0 4px 10px rgba(0,0,0,0.5); }
        select { background: #333; color: #fff; border: 1px solid #555; padding: 10px; border-radius: 4px; width: 100%; margin-bottom: 20px; font-size: 1.1rem; }
        .grid-48 { display: grid; grid-template-columns: repeat(auto-fill, minmax(110px, 1fr)); gap: 10px; }
        .chk-box { background: #2a2a2a; padding: 10px; border-radius: 6px; display: flex; align-items: center; cursor: pointer; border: 1px solid #444; }
        .chk-box:hover { background: #3a3a3a; }
        .chk-box input { margin-right: 10px; transform: scale(1.3); cursor: pointer; }
        .buttons { display: flex; gap: 15px; margin-top: 25px; }
        button { flex: 1; background: var(--accent); color: #000; border: none; padding: 15px; border-radius: 4px; cursor: pointer; font-weight: bold; font-size: 1.1rem; }
        button:hover { background: #0097a7; }
        .btn-back { background: #555; color: white; }
        .btn-back:hover { background: #666; }
        .toast { visibility: hidden; background-color: #4CAF50; color: white; text-align: center; border-radius: 4px; padding: 16px; position: fixed; bottom: 30px; left: 50%; transform: translateX(-50%); z-index: 1; }
        .toast.show { visibility: visible; animation: fadein 0.5s, fadeout 0.5s 2.5s; }
        @keyframes fadein { from {bottom: 0; opacity: 0;} to {bottom: 30px; opacity: 1;} }
        @keyframes fadeout { from {bottom: 30px; opacity: 1;} to {bottom: 0; opacity: 0;} }
    </style>
</head>
<body>
    <h1>24-Hour Timer Schedule</h1>
    <div class="card">
        <label>Select Channel:</label>
        <select id="chSel" onchange="loadSchedule()"></select>
        
        <div class="grid-48" id="grid"></div>

        <div class="buttons">
            <button class="btn-back" onclick="location.href='/'">🔙 Back</button>
            <button onclick="saveSchedule()">💾 Save Schedule</button>
        </div>
    </div>
    <div id="toast" class="toast">Schedule Saved!</div>

    <script>
        let grid = document.getElementById('grid');
        let html = '';
        for(let i=0; i<48; i++) {
            let h = Math.floor(i / 2);
            let m = (i % 2 === 0) ? '00' : '30';
            let hh = (h < 10 ? '0' : '') + h;
            html += `<label class="chk-box"><input type="checkbox" id="cb${i}"> ${hh}:${m}</label>`;
        }
        grid.innerHTML = html;

        function showToast() {
            let el = document.getElementById("toast");
            el.className = "toast show";
            setTimeout(() => { el.className = el.className.replace("toast show", "toast"); }, 3000);
        }

        function loadSchedule() {
            let ch = document.getElementById('chSel').value;
            fetch('/api/get_schedule?ch=' + ch)
                .then(r => r.json())
                .then(data => {
                    for(let i=0; i<48; i++) {
                        document.getElementById('cb'+i).checked = (data[i] == 1);
                    }
                });
        }

        function saveSchedule() {
            let ch = document.getElementById('chSel').value;
            let strData = '';
            for(let i=0; i<48; i++) {
                strData += document.getElementById('cb'+i).checked ? '1' : '0';
            }
            let p = new URLSearchParams();
            p.append('ch', ch);
            p.append('data', strData);
            fetch('/api/set_schedule', { method: 'POST', body: p }).then(() => showToast());
        }

        window.onload = function() {
            fetch('/api/state').then(r => r.json()).then(data => {
                let sel = document.getElementById('chSel');
                for(let i=0; i<4; i++) {
                    let opt = document.createElement('option');
                    opt.value = i;
                    opt.text = data.rName[i];
                    sel.appendChild(opt);
                }
                loadSchedule();
            });
        };
    </script>
</body>
</html>
)rawliteral";


// ==============================================================================
// API HANDLERS & ROUTING
// ==============================================================================

void handleRoot() { server.send(200, "text/html", index_html); }
void handleSchedulePage() { server.send(200, "text/html", schedule_html); }

void handleGetState() {
    String json = "{";
    json += "\"ssid\":\"" + ssid + "\",";
    json += "\"deviceId\":\"" + mqttDeviceId() + "\",";
    json += "\"mqttEnabled\":" + String(mqttEnabled ? "true" : "false") + ",";
    json += "\"mqttConnected\":" + String(isMqttServiceConnected() ? "true" : "false") + ",";
    json += "\"mqttDetail\":\"" + mqttServiceStatus() + "\",";
    json += "\"gmt\":" + String(gmtOffset_sec / 3600) + ",";
    json += "\"cH\":" + String(currentHour) + ",";
    json += "\"cM\":" + String(currentMinute) + ",";
    
    json += "\"tW\":" + String(tempWater, 1) + ",";
    json += "\"tH\":" + String(tempHeatsink, 1) + ",";
    json += "\"fan\":" + String(isFanOn ? "true" : "false") + ",";

    const char* lightState = "off";
    switch (currentLightState) {
        case MOON: lightState = "moon"; break;
        case SUNRISE: lightState = "sunrise"; break;
        case DAY: lightState = "day"; break;
        case PAUSE_SUNSET:
        case PAUSE:
        case PAUSE_SUNRISE: lightState = "pause"; break;
        case SUNSET: lightState = "sunset"; break;
        case OFF: lightState = "off"; break;
    }
    json += "\"lightState\":\"" + String(lightState) + "\",";
    json += "\"activeLight\":[" + String(currentBrightness[0]) + "," + String(currentBrightness[1]) + "," + String(currentBrightness[2]) + "," + String(currentBrightness[3]) + "," + String(currentBrightness[4]) + "],";
    
    // Light and Moon Data
    json += "\"l\":[" + String(brightnessSettings[0]) + "," + String(brightnessSettings[1]) + "," + String(brightnessSettings[2]) + "," + String(brightnessSettings[3]) + "," + String(brightnessSettings[4]) + "],";
    json += "\"m\":[" + String(moonlightSettings[0]) + "," + String(moonlightSettings[1]) + "," + String(moonlightSettings[2]) + "," + String(moonlightSettings[3]) + "," + String(moonlightSettings[4]) + "],";
    
    // Light Timers
    json += "\"lOnH\":" + String(lightOnHour) + ",";
    json += "\"lOnM\":" + String(lightOnMinute) + ",";
    json += "\"lOffH\":" + String(lightOffHour) + ",";
    json += "\"lOffM\":" + String(lightOffMinute) + ",";
    json += "\"fade\":" + String(sunriseDuration) + ",";
    json += "\"moonEn\":" + String(isMoonlightEnabled ? "true" : "false") + ",";
    json += "\"pStH\":" + String(dayPauseStartHour) + ",";
    json += "\"pStM\":" + String(dayPauseStartMinute) + ",";
    json += "\"pEnH\":" + String(dayPauseEndHour) + ",";
    json += "\"pEnM\":" + String(dayPauseEndMinute) + ",";

    json += "\"fOn\":" + String(fanOnTemp, 1) + ",";
    json += "\"fMax\":" + String(maxFanTemp, 1) + ",";
    json += "\"fStart\":" + String(fanStartLevel) + ",";
    json += "\"fHys\":" + String(fanHysteresis, 1) + ",";
    
    // Relay Data
    json += "\"rName\":[\"" + relayNames[0] + "\",\"" + relayNames[1] + "\",\"" + relayNames[2] + "\",\"" + relayNames[3] + "\"],";
    json += "\"rCtrl\":[" + String(controlType[0]) + "," + String(controlType[1]) + "," + String(controlType[2]) + "," + String(controlType[3]) + "],";
    json += "\"rSP\":[" + String(setPoint[0], 1) + "," + String(setPoint[1], 1) + "," + String(setPoint[2], 1) + "," + String(setPoint[3], 1) + "],";
    json += "\"rHys\":[" + String(hysteresis[0], 1) + "," + String(hysteresis[1], 1) + "," + String(hysteresis[2], 1) + "," + String(hysteresis[3], 1) + "],";
    json += "\"rState\":[" + String(relayState[0] ? "true" : "false") + "," + String(relayState[1] ? "true" : "false") + "," + String(relayState[2] ? "true" : "false") + "," + String(relayState[3] ? "true" : "false") + "],";
    
    // Doser Data
    json += "\"dName\":[\"" + doserNames[0] + "\",\"" + doserNames[1] + "\",\"" + doserNames[2] + "\",\"" + doserNames[3] + "\"],";
    json += "\"dHr\":[" + String(fertTriggerHour[0]) + "," + String(fertTriggerHour[1]) + "," + String(fertTriggerHour[2]) + "," + String(fertTriggerHour[3]) + "],";
    json += "\"dMin\":[" + String(fertTriggerMinute[0]) + "," + String(fertTriggerMinute[1]) + "," + String(fertTriggerMinute[2]) + "," + String(fertTriggerMinute[3]) + "],";
    json += "\"dRun\":[" + String(fertMotorRunTime[0]) + "," + String(fertMotorRunTime[1]) + "," + String(fertMotorRunTime[2]) + "," + String(fertMotorRunTime[3]) + "],";
    json += "\"dEnabled\":[" + String(fertEnabled[0] ? "true" : "false") + "," + String(fertEnabled[1] ? "true" : "false") + "," + String(fertEnabled[2] ? "true" : "false") + "," + String(fertEnabled[3] ? "true" : "false") + "],";
    json += "\"dDays\":[";
    for (int channel = 0; channel < 4; channel++) {
        if (channel > 0) json += ",";
        json += "[";
        for (int day = 0; day < 7; day++) {
            if (day > 0) json += ",";
            json += fertDays[channel][day] ? "true" : "false";
        }
        json += "]";
    }
    json += "],";
    
    // Feeder Data
    json += "\"fd1H\":" + String(feed1Hour) + ",";
    json += "\"fd1M\":" + String(feed1Minute) + ",";
    json += "\"fd2H\":" + String(feed2Hour) + ",";
    json += "\"fd2M\":" + String(feed2Minute) + ",";
    json += "\"fdDur\":" + String(feedDuration) + ",";
    json += "\"fdPause\":" + String(feedFilterPauseEnabled ? "true" : "false");
    json += "}";
    apiSend(200, "application/json", json);
}

void handleGetMqttSettings() {
    JsonDocument settings;
    settings["enabled"] = mqttEnabled;
    settings["host"] = mqttHost;
    settings["port"] = mqttPort;
    settings["username"] = mqttUsername;
    settings["tls"] = mqttUseTls;
    settings["ca"] = mqttCaCertificate;
    settings["customDeviceId"] = mqttCustomDeviceId;
    settings["deviceId"] = mqttDeviceId();
    String json;
    serializeJson(settings, json);
    apiSend(200, "application/json", json);
}

void handleGetSchedule() {
    if (!apiHasArg("ch")) { apiSend(400); return; }
    int ch = apiArg("ch").toInt();
    if (ch >= 0 && ch < 4) {
        String json = "[";
        for (int i = 0; i < 48; i++) {
            json += schedule[ch][i] ? "1" : "0";
            if (i < 47) json += ",";
        }
        json += "]";
        apiSend(200, "application/json", json);
    } else apiSend(400, "text/plain", "Invalid schedule channel");
}

void handleSetSchedule() {
    if (apiHasArg("ch") && apiHasArg("data")) {
        int ch = apiArg("ch").toInt();
        String data = apiArg("data");
        if (ch >= 0 && ch < 4 && data.length() == 48) {
            for (int i = 0; i < 48; i++) {
                schedule[ch][i] = (data[i] == '1');
            }
            saveRelaySchedule();
            apiSend(200, "text/plain", "OK");
            return;
        }
    }
    apiSend(400, "text/plain", "Bad Data");
}

void handleSetSystem() {
    if (apiHasArg("ssid")) ssid = apiArg("ssid");
    if (apiHasArg("pass") && apiArg("pass").length() > 0) password = apiArg("pass");
    if (apiHasArg("gmt")) gmtOffset_sec = apiArg("gmt").toInt() * 3600;
    saveSystemSettings();
    apiSend(200, "text/plain", "OK");
    if (!processingMqttRequest) { delay(500); ESP.restart(); }
}

void handleSetMqttSettings() {
    const String enabled = apiArg("enabled");
    const String host = apiArg("host");
    const int port = apiArg("port").toInt();
    String customDeviceId = mqttCustomDeviceId;
    if (apiHasArg("deviceId")) customDeviceId = apiArg("deviceId");
    customDeviceId.trim();
    for (size_t i = 0; i < customDeviceId.length(); i++) {
        const char c = customDeviceId[i];
        const bool validCharacter =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!validCharacter) {
            apiSend(400, "text/plain", "Device ID may contain only letters, digits, hyphens, and underscores");
            return;
        }
    }
    if (customDeviceId.length() > 32) {
        apiSend(400, "text/plain", "Device ID must be 32 characters or fewer");
        return;
    }
    if (enabled != "0" && enabled != "1") {
        apiSend(400, "text/plain", "Invalid MQTT enabled value");
        return;
    }
    if (enabled == "1" && (host.isEmpty() || port < 1 || port > 65535)) {
        apiSend(400, "text/plain", "MQTT broker host and valid port are required");
        return;
    }
    String caCertificate = apiArg("ca");
    caCertificate.trim();
    if (caCertificate.isEmpty()) caCertificate = mqttCaCertificate;
    const bool useTls = apiArg("tls") == "1";
    if (enabled == "1" && useTls &&
        (caCertificate.length() > 3500 ||
         !caCertificate.startsWith("-----BEGIN CERTIFICATE-----") ||
         !caCertificate.endsWith("-----END CERTIFICATE-----"))) {
        apiSend(400, "text/plain", "A single PEM broker CA certificate is required for TLS");
        return;
    }

    mqttEnabled = enabled == "1";
    mqttCustomDeviceId = customDeviceId;
    mqttHost = host;
    mqttPort = static_cast<uint16_t>(port);
    mqttUsername = apiArg("username");
    if (apiHasArg("clearPassword")) mqttPassword = "";
    else if (apiHasArg("password")) mqttPassword = apiArg("password");
    mqttUseTls = useTls;
    mqttCaCertificate = caCertificate;
    saveMqttSettings();
    apiSend(200, "text/plain", "OK");
}

void handleSetTime() {
    if (apiHasArg("h") && apiHasArg("m")) {
        struct tm t = {0};
        time_t now; time(&now); localtime_r(&now, &t); 
        t.tm_hour = apiArg("h").toInt();
        t.tm_min = apiArg("m").toInt();
        t.tm_sec = apiHasArg("s") ? apiArg("s").toInt() : 0;
        if (t.tm_year < 120) { t.tm_year = 126; t.tm_mon = 0; t.tm_mday = 1; }
        
        time_t timeSinceEpoch = mktime(&t);
        struct timeval tv = { .tv_sec = (long)timeSinceEpoch, .tv_usec = 0 };
        settimeofday(&tv, NULL); 
        apiSend(200, "text/plain", "OK");
    } else {
        apiSend(400, "text/plain", "Bad Request");
    }
}

void handleSetLight() {
    if (apiHasArg("ch") && apiHasArg("val")) {
        int ch = apiArg("ch").toInt();
        int val = apiArg("val").toInt();
        if (ch >= 0 && ch < 5) {
            brightnessSettings[ch] = constrain(val, 0, 100);
            saveLightBrightness(); 
            if (isLightOn) setPWMOutputs(brightnessSettings);
        }
    }
    apiSend(200, "text/plain", "OK");
}

void handleSetMoon() {
    if (apiHasArg("ch") && apiHasArg("val")) {
        int ch = apiArg("ch").toInt();
        int val = apiArg("val").toInt();
        if (ch >= 0 && ch < 5) {
            moonlightSettings[ch] = constrain(val, 0, 100);
            saveMoonBrightness(); 
            if (!isLightOn) setPWMOutputs(moonlightSettings); 
        }
    }
    apiSend(200, "text/plain", "OK");
}

void handleSetLightTimers() {
    if (apiHasArg("onH")) {
        lightOnHour = apiArg("onH").toInt();
        lightOnMinute = apiArg("onM").toInt();
        lightOffHour = apiArg("offH").toInt();
        lightOffMinute = apiArg("offM").toInt();
        sunriseDuration = apiArg("fade").toInt();
        isMoonlightEnabled = (apiArg("mEn") == "1");
        if (!isLightOn && (currentLightState == MOON || currentLightState == OFF)) {
            if (isMoonlightEnabled) {
                currentLightState = MOON;
                setPWMOutputs(moonlightSettings);
            } else {
                currentLightState = OFF;
                turnOffAllChannels();
            }
        }
        dayPauseStartHour = apiArg("pStH").toInt();
        dayPauseStartMinute = apiArg("pStM").toInt();
        dayPauseEndHour = apiArg("pEnH").toInt();
        dayPauseEndMinute = apiArg("pEnM").toInt();
        
        saveLightTimers();
        apiSend(200, "text/plain", "OK");
    } else {
        apiSend(400, "text/plain", "Bad Request");
    }
}

void handleSetFan() {
    if (apiHasArg("on")) fanOnTemp = apiArg("on").toFloat();
    if (apiHasArg("max")) maxFanTemp = apiArg("max").toFloat();
    if (apiHasArg("start")) fanStartLevel = apiArg("start").toInt();
    if (apiHasArg("hys")) fanHysteresis = apiArg("hys").toFloat();
    saveFanSettings(); apiSend(200, "text/plain", "OK");
}

void handleSetRelay() {
    if (apiHasArg("ch")) {
        int ch = apiArg("ch").toInt();
        if (ch >= 0 && ch < 4) {
            if (apiHasArg("name")) relayNames[ch] = apiArg("name");
            if (apiHasArg("mode")) controlType[ch] = apiArg("mode").toInt();
            if (apiHasArg("sp")) setPoint[ch] = apiArg("sp").toFloat();
            if (apiHasArg("hys")) hysteresis[ch] = apiArg("hys").toFloat();
            saveRelaySettings();
        }
    }
    apiSend(200, "text/plain", "OK");
}

void handleSetDoser() {
    if (!apiHasArg("ch") || !apiHasArg("h") || !apiHasArg("m") || !apiHasArg("run")) {
        apiSend(400, "text/plain", "Missing doser settings");
        return;
    }

    int ch = apiArg("ch").toInt();
    String hourArg = apiArg("h");
    String minuteArg = apiArg("m");
    int hour = hourArg.toInt();
    int minute = minuteArg.toInt();
    float runSeconds = apiArg("run").toFloat();

    bool validHour = hourArg.length() > 0;
    bool validMinute = minuteArg.length() > 0;
    for (size_t i = 0; i < hourArg.length(); i++) {
        if (hourArg[i] < '0' || hourArg[i] > '9') validHour = false;
    }
    for (size_t i = 0; i < minuteArg.length(); i++) {
        if (minuteArg[i] < '0' || minuteArg[i] > '9') validMinute = false;
    }
    if (ch < 0 || ch >= 4) {
        apiSend(400, "text/plain", "Invalid doser channel");
        return;
    }
    if (!validHour || hour < 0 || hour > 23 || !validMinute || minute < 0 || minute > 59) {
        apiSend(400, "text/plain", "Invalid dosing time");
        return;
    }
    if (!(runSeconds >= 0.1f && runSeconds <= 30.0f)) {
        apiSend(400, "text/plain", "Run time must be between 0.1 and 30 seconds");
        return;
    }

    fertTriggerHour[ch] = static_cast<uint8_t>(hour);
    fertTriggerMinute[ch] = static_cast<uint8_t>(minute);
    fertMotorRunTime[ch] = static_cast<unsigned long>(runSeconds * 10.0f + 0.5f);
    if (apiHasArg("name")) doserNames[ch] = apiArg("name");
    if (apiHasArg("enabled")) fertEnabled[ch] = apiArg("enabled") == "1";
    for (int day = 0; day < 7; day++) {
        String dayArg = "day" + String(day);
        if (apiHasArg(dayArg)) fertDays[ch][day] = apiArg(dayArg) == "1";
    }
    saveDoserSettings();
    apiSend(200, "text/plain", "OK");
}

void handleSetFeeder() {
    if (apiHasArg("dur")) {
        feed1Hour = apiArg("f1H").toInt();
        feed1Minute = apiArg("f1M").toInt();
        feed2Hour = apiArg("f2H").toInt();
        feed2Minute = apiArg("f2M").toInt();
        feedDuration = apiArg("dur").toInt();
        if (apiHasArg("filterPause")) {
            feedFilterPauseEnabled = apiArg("filterPause") == "1";
        }
        saveFeederSettings(); apiSend(200, "text/plain", "OK");
    } else apiSend(400, "text/plain", "Missing feeder settings");
}

void handleManualFeed() { manualFeed(); apiSend(200, "text/plain", "OK"); }

bool dispatchMqttApiRequest(
    const String& method,
    const String& path,
    JsonObjectConst args,
    int& statusCode,
    String& contentType,
    String& responseBody
) {
    if (processingMqttRequest) return false;
    processingMqttRequest = true;
    mqttRequestArgs = args;
    mqttResponseStatus = 500;
    mqttResponseType = "text/plain";
    mqttResponseBody = "";

    const bool isGet = method == "GET";
    const bool isPost = method == "POST";
    if (isGet && path == "/api/state") handleGetState();
    else if (isGet && path == "/api/mqtt") handleGetMqttSettings();
    else if (isGet && path == "/api/get_schedule") handleGetSchedule();
    else if (isPost && path == "/api/set_time") handleSetTime();
    else if (isPost && path == "/api/set_schedule") handleSetSchedule();
    else if (isPost && path == "/api/set_sys") handleSetSystem();
    else if (isPost && path == "/api/set_mqtt") handleSetMqttSettings();
    else if (isPost && path == "/api/set_light") handleSetLight();
    else if (isPost && path == "/api/set_moon") handleSetMoon();
    else if (isPost && path == "/api/set_light_timers") handleSetLightTimers();
    else if (isPost && path == "/api/set_fan") handleSetFan();
    else if (isPost && path == "/api/set_relay") handleSetRelay();
    else if (isPost && path == "/api/set_doser") handleSetDoser();
    else if (isPost && path == "/api/set_feeder") handleSetFeeder();
    else if (isPost && path == "/api/manual_feed") handleManualFeed();
    else {
        processingMqttRequest = false;
        return false;
    }

    statusCode = mqttResponseStatus;
    contentType = mqttResponseType;
    responseBody = mqttResponseBody;
    processingMqttRequest = false;
    return true;
}

void handleMonitorOtaUploadFile() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        monitorFirmwareUploadOk = SPIFFS.begin(true);
        monitorFirmwareUploadBytes = 0;
        if (!monitorFirmwareUploadOk) return;
        File file = SPIFFS.open("/monitor.bin", FILE_WRITE);
        if (file) file.close();
        else monitorFirmwareUploadOk = false;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!monitorFirmwareUploadOk) return;
        File file = SPIFFS.open("/monitor.bin", FILE_APPEND);
        if (file) {
            size_t written = file.write(upload.buf, upload.currentSize);
            file.close();
            if (written == upload.currentSize) monitorFirmwareUploadBytes += written;
            else monitorFirmwareUploadOk = false;
        } else monitorFirmwareUploadOk = false;
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Monitor firmware uploaded: %u bytes\n", upload.totalSize);
        if (monitorFirmwareUploadBytes != upload.totalSize) monitorFirmwareUploadOk = false;
    }
}

void handleMonitorOtaUploadComplete() {
    if (!monitorFirmwareUploadOk || monitorFirmwareUploadBytes == 0) {
        server.send(500, "text/plain", "Monitor firmware upload failed");
        return;
    }
    server.send(200, "text/plain", "Monitor firmware uploaded");
}

void finishMonitorOtaTask(uint8_t state, uint8_t error) {
    setMonitorOtaTransferActive(false);
    monitorOtaError = error;
    monitorOtaState = state;
    vTaskDelete(nullptr);
}

void monitorOtaTransferTask(void *parameter) {
    File file = SPIFFS.open("/monitor.bin", "r");
    if (!file) {
        finishMonitorOtaTask(MONITOR_OTA_FAILED, 1);
        return;
    }

    monitorOtaTotal = file.size();
    if (monitorOtaTotal == 0) {
        file.close();
        finishMonitorOtaTask(MONITOR_OTA_FAILED, 2);
        return;
    }

    if (!sendMonitorOtaStart(monitorOtaTotal)) {
        file.close();
        finishMonitorOtaTask(MONITOR_OTA_FAILED, 3);
        return;
    }

    uint16_t seq = 0;
    uint8_t buffer[200];
    while (file.available()) {
        size_t len = file.read(buffer, sizeof(buffer));
        if (len == 0) {
            file.close();
            finishMonitorOtaTask(MONITOR_OTA_FAILED, 4);
            return;
        }
        if (!sendMonitorOtaData(seq, buffer, (uint16_t)len)) {
            file.close();
            finishMonitorOtaTask(MONITOR_OTA_FAILED, 5);
            return;
        }
        monitorOtaSent += len;
        seq++;
    }

    file.close();
    if (monitorOtaSent != monitorOtaTotal || !sendMonitorOtaEnd()) {
        finishMonitorOtaTask(MONITOR_OTA_FAILED, 6);
        return;
    }

    finishMonitorOtaTask(MONITOR_OTA_COMPLETE, 0);
}

void handleMonitorOtaRequest() {
    if (monitorOtaState == MONITOR_OTA_TRANSFER) {
        server.send(409, "text/plain", "Monitor OTA is already running");
        return;
    }
    if (!SPIFFS.exists("/monitor.bin")) {
        server.send(400, "text/plain", "No monitor firmware uploaded");
        return;
    }

    monitorOtaSent = 0;
    monitorOtaTotal = 0;
    monitorOtaError = 0;
    monitorOtaState = MONITOR_OTA_TRANSFER;
    setMonitorOtaTransferActive(true);
    if (xTaskCreate(monitorOtaTransferTask, "monitorOta", 4096, nullptr, 1, nullptr) != pdPASS) {
        setMonitorOtaTransferActive(false);
        monitorOtaState = MONITOR_OTA_FAILED;
        monitorOtaError = 1;
        server.send(500, "text/plain", "Cannot start monitor OTA task");
        return;
    }
    server.send(202, "text/plain", "Monitor OTA started");
}

void handleMonitorOtaStatus() {
    char response[96];
    snprintf(response, sizeof(response), "{\"state\":%u,\"sent\":%lu,\"total\":%lu,\"error\":%u}",
             (unsigned int)monitorOtaState,
             (unsigned long)monitorOtaSent,
             (unsigned long)monitorOtaTotal,
             (unsigned int)monitorOtaError);
    server.send(200, "application/json", response);
}

// Initialization
void initWebServer() {
    SPIFFS.begin(true);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/schedule", HTTP_GET, handleSchedulePage);

    server.on("/api/state", HTTP_GET, handleGetState);
    server.on("/api/mqtt", HTTP_GET, handleGetMqttSettings);
    server.on("/api/get_schedule", HTTP_GET, handleGetSchedule);

    server.on("/api/set_time", HTTP_POST, handleSetTime);
    server.on("/api/set_schedule", HTTP_POST, handleSetSchedule);
    server.on("/api/set_sys", HTTP_POST, handleSetSystem);
    server.on("/api/set_mqtt", HTTP_POST, handleSetMqttSettings);
    server.on("/api/set_light", HTTP_POST, handleSetLight);
    server.on("/api/set_moon", HTTP_POST, handleSetMoon);
    server.on("/api/set_light_timers", HTTP_POST, handleSetLightTimers);
    server.on("/api/set_fan", HTTP_POST, handleSetFan);
    server.on("/api/set_relay", HTTP_POST, handleSetRelay);
    server.on("/api/set_doser", HTTP_POST, handleSetDoser);
    server.on("/api/set_feeder", HTTP_POST, handleSetFeeder);
    server.on("/api/manual_feed", HTTP_POST, handleManualFeed);
    server.on("/api/monitor_ota", HTTP_POST, handleMonitorOtaRequest);
    server.on("/api/monitor_ota_status", HTTP_GET, handleMonitorOtaStatus);
    server.on("/api/monitor_ota_upload", HTTP_POST, handleMonitorOtaUploadComplete, handleMonitorOtaUploadFile);

    if (MDNS.begin("aquaos")) {
        Serial.println("mDNS responder started: http://aquaos.local");
    } else {
        Serial.println("Error setting up MDNS responder!");
    }

    ElegantOTA.begin(&server);
    server.begin();
    Serial.println("Web server started!");
}

void handleClientWebServer() {
    server.handleClient();
    ElegantOTA.loop();
}
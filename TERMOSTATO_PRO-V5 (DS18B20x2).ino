
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "ArduinoJson.h"
#include <ESP8266mDNS.h>
#include <Updater.h>

// ------------------- PINES -------------------
const int DS18B20_PIN_1 = 2;   // Sensor 1 (Pin D4 -> GPIO 2)
const int DS18B20_PIN_2 = 14;  // Sensor 2 (Pin D5 -> GPIO 14)
const int RELAY_PIN     = 0;   // Relé (Pin D3 -> GPIO 0)

// ------------------- Configuración -------------------
struct ConfigStruct {
  uint16_t magic;
  char wifiSSID[32];
  char wifiPASS[32];
  unsigned long sampleInterval;
  double targetTemp;
  double offsetTempDS18B20_1;
  double offsetTempDS18B20_2;
  uint8_t sensorControl;   // 0=S1, 1=S2, 2=Promedio
  double alertMaxTemp;
  double alertMinTemp;
  bool   alertsEnabled;
  char   nodeIPs[40];       // IPs de otros nodos, separadas por coma
} config;

void setDefaultConfig() {
  config.magic = 0xBE07;
  strcpy(config.wifiSSID, "NOMBRE_DE_TU_WIFI");
  strcpy(config.wifiPASS, "PASS_WIFI");
  config.sampleInterval       = 10000;
  config.targetTemp           = 60.0;
  config.offsetTempDS18B20_1 = 0.0;
  config.offsetTempDS18B20_2 = 0.0;
  config.sensorControl        = 0;
  config.alertMaxTemp         = 80.0;
  config.alertMinTemp         = 5.0;
  config.alertsEnabled        = true;
  strcpy(config.nodeIPs, "192.168.1.81");
}

// ------------------- Sensores, LCD y NTP -------------------
OneWire oneWireBus1(DS18B20_PIN_1);
OneWire oneWireBus2(DS18B20_PIN_2);
DallasTemperature sensor1(&oneWireBus1);
DallasTemperature sensor2(&oneWireBus2);

LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -10800, 60000);

// ------------------- Servidor -------------------
AsyncWebServer server(80);
bool isAPMode = false;

// ------------------- Histórico -------------------
#define MAX_HISTORY 50
double tempHistory1[MAX_HISTORY];
double tempHistory2[MAX_HISTORY];
unsigned long timeHistory[MAX_HISTORY];
int historyIndex = 0;

// ------------------- Variables -------------------
unsigned long lastSample      = 0;
bool rebootNeeded             = false;
bool manualControl            = false;
bool relayState               = false;
double lastTemp1              = 0.0;
double lastTemp2              = 0.0;
bool alertActive              = false;
String alertMessage           = "";
uint16_t eepromWrites         = 0;

// ------------------- EEPROM -------------------
// Guarda solo si algo realmente cambió (protege contra desgaste de la flash)
bool saveConfig() {
  EEPROM.begin(sizeof(ConfigStruct));
  ConfigStruct stored;
  EEPROM.get(0, stored);

  if (memcmp(&stored, &config, sizeof(ConfigStruct)) == 0) {
    // Nada cambió -> no escribir
    EEPROM.end();
    Serial.println("EEPROM: sin cambios, escritura omitida.");
    return false;
  }

  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
  eepromWrites++;
  Serial.printf("EEPROM: guardado (#%u escrituras totales)\n", eepromWrites);
  return true;
}

void loadConfig() {
  EEPROM.begin(sizeof(ConfigStruct));
  EEPROM.get(0, config);
  EEPROM.end();
  if (config.magic != 0xBE07 || strlen(config.wifiSSID) < 5 || strlen(config.wifiPASS) < 8) {
    setDefaultConfig();
  }
}

// ------------------- Histórico -------------------
void addHistory(double t1, double t2) {
  long epoch = 0;
  if (WiFi.status() == WL_CONNECTED) {
    epoch = timeClient.getEpochTime();
  }
  // Si NTP no sincronizó aún o devolvió 0, usar contador interno en base al inicio + offset
  if (epoch < 10000) {
    epoch = (millis() / 1000) + 10; // Nunca será 0
  }
  timeHistory[historyIndex] = epoch;
  tempHistory1[historyIndex] = t1;
  tempHistory2[historyIndex] = t2;
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
}

// =========================================================
//  SETUP
// =========================================================
void setup() {
  Serial.begin(115200);
  lcd.init(); lcd.backlight();
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  sensor1.begin();
  sensor2.begin();

  loadConfig();

  // Bienvenida
  lcd.setCursor(2, 0); lcd.print("Byte-E.LAB");
  lcd.setCursor(4, 1); lcd.print("v.5.2 Dual");
  delay(3000);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Conectando WiFi");

  // WiFi
  WiFi.begin(config.wifiSSID, config.wifiPASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi Conectado");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    Serial.println(WiFi.localIP());
    isAPMode = false;
    timeClient.begin();
    timeClient.update();
    if (MDNS.begin("termostato")) {
      Serial.println("mDNS: http://termostato.local");
    }
    Serial.println("Servidor y OTA web listos.");

  } else {
    isAPMode = true;
    lcd.clear();
    lcd.print("Fallo WiFi");
    lcd.setCursor(0, 1); lcd.print("Modo AP");
    WiFi.softAP("NodeMCU-Config");
    Serial.print("IP AP: "); Serial.println(WiFi.softAPIP());
  }

  // Lectura inicial
  sensor1.requestTemperatures();
  sensor2.requestTemperatures();
  lastTemp1 = sensor1.getTempCByIndex(0) + config.offsetTempDS18B20_1;
  lastTemp2 = sensor2.getTempCByIndex(0) + config.offsetTempDS18B20_2;
  
  addHistory(lastTemp1, lastTemp2);

  // =========================================================
  //  RUTA: / (Dashboard principal)
  // =========================================================
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    static const char page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <title>Termostato Dual DS18B20</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;700&display=swap');

    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

    body {
      font-family: 'Inter', sans-serif;
      background: linear-gradient(135deg, #0f0f1a 0%, #1a1a2e 50%, #16213e 100%);
      color: #e0e6ed;
      min-height: 100vh;
      padding: 20px;
      display: flex;
      flex-direction: column;
      align-items: center;
    }

    h1 {
      font-size: 1.8em;
      font-weight: 700;
      background: linear-gradient(90deg, #00aaff, #00ffcc);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      margin-bottom: 6px;
    }

    .subtitle {
      color: #7a8a9a;
      font-size: 0.85em;
      margin-bottom: 20px;
    }

    .container {
      background: rgba(255,255,255,0.04);
      backdrop-filter: blur(12px);
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 16px;
      padding: 24px;
      width: 100%;
      max-width: 700px;
      box-shadow: 0 8px 32px rgba(0,0,0,0.4);
      margin-bottom: 20px;
    }

    /* ---- GAUGES ---- */
    .gauges-row {
      display: flex;
      justify-content: space-around;
      gap: 20px;
      flex-wrap: wrap;
      margin-bottom: 20px;
    }

    .gauge-card {
      display: flex;
      flex-direction: column;
      align-items: center;
      flex: 1;
      min-width: 160px;
    }

    .gauge-title {
      font-size: 0.82em;
      font-weight: 600;
      letter-spacing: 1px;
      text-transform: uppercase;
      margin-bottom: 8px;
    }

    .s1-color { color: #00aaff; }
    .s2-color { color: #ff6b6b; }

    .gauge {
      width: 140px;
      height: 140px;
      border-radius: 50%;
      border: 8px solid rgba(255,255,255,0.08);
      position: relative;
      overflow: hidden;
    }

    .gauge-bg-1 { background: linear-gradient(to top, #1a6fff 0%, #a78bfa 100%); }
    .gauge-bg-2 { background: linear-gradient(to top, #ff416c 0%, #ff4b2b 100%); }

    .gauge-fill {
      position: absolute;
      bottom: 0; left: 0;
      width: 100%;
      background: #1a1a2e;
      transition: height 0.6s cubic-bezier(0.4,0,0.2,1);
    }

    .gauge-label {
      position: absolute;
      top: 50%; left: 50%;
      transform: translate(-50%, -50%);
      font-size: 1.6em;
      font-weight: 700;
      color: #fff;
      text-shadow: 0 0 10px rgba(0,0,0,0.6);
      white-space: nowrap;
    }

    /* ---- STATS ---- */
    .stats-row {
      display: flex;
      gap: 10px;
      justify-content: center;
      flex-wrap: wrap;
      margin-bottom: 20px;
    }

    .stat-card {
      background: rgba(255,255,255,0.06);
      border: 1px solid rgba(255,255,255,0.1);
      border-radius: 10px;
      padding: 10px 14px;
      text-align: center;
      min-width: 90px;
    }

    .stat-card p {
      font-size: 0.72em;
      color: #7a8a9a;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }

    .stat-card h3 {
      font-size: 1.1em;
      font-weight: 600;
      margin-top: 4px;
    }

    .stat-s1 { border-top: 3px solid #00aaff; }
    .stat-s2 { border-top: 3px solid #ff6b6b; }

    /* ---- CHART ---- */
    .chart-container {
      position: relative;
      height: 260px;
      width: 100%;
      margin-bottom: 16px;
    }

    canvas {
      border-radius: 10px;
    }

    /* ---- RELAY CONTROL ---- */
    .section-title {
      font-size: 0.75em;
      letter-spacing: 1.5px;
      text-transform: uppercase;
      color: #7a8a9a;
      margin-bottom: 12px;
      padding-top: 12px;
      border-top: 1px solid rgba(255,255,255,0.07);
    }

    .control-buttons {
      display: flex;
      justify-content: center;
      gap: 10px;
      flex-wrap: wrap;
      margin-bottom: 12px;
    }

    .control-buttons button {
      padding: 10px 20px;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      font-size: 0.9em;
      font-weight: 600;
      font-family: 'Inter', sans-serif;
      transition: transform 0.1s, opacity 0.2s;
    }

    .control-buttons button:active { transform: scale(0.96); }
    .btn-on  { background: linear-gradient(135deg, #00b09b, #2ecc71); color: #fff; }
    .btn-off { background: linear-gradient(135deg, #e74c3c, #c0392b); color: #fff; }
    .btn-auto{ background: linear-gradient(135deg, #3498db, #1a6fff); color: #fff; }
    .control-buttons button:hover { opacity: 0.85; }

    .status-badge {
      display: inline-block;
      padding: 6px 14px;
      border-radius: 20px;
      font-size: 0.85em;
      font-weight: 600;
      margin-top: 4px;
      background: rgba(255,255,255,0.08);
    }

    .relay-on  { background: rgba(46,204,113,0.2); color: #2ecc71; border: 1px solid #2ecc71; }
    .relay-off { background: rgba(231,76,60,0.2);  color: #e74c3c; border: 1px solid #e74c3c; }
    .relay-auto{ background: rgba(52,152,219,0.2); color: #3498db; border: 1px solid #3498db; }

    nav {
      display: flex;
      gap: 16px;
      margin-top: 8px;
    }

    nav a {
      color: #00aaff;
      text-decoration: none;
      font-size: 0.9em;
      font-weight: 600;
      transition: color 0.2s;
    }

    nav a:hover { color: #00ffcc; }
  </style>
</head>
<body>
  <h1>Termostato Dual DS18B20</h1>
  <p class="subtitle" id="ts-label">Cargando...</p>
  <p class="subtitle" style="color:#00aaff;font-size:0.8em;">v5.2 | Byte-E.LAB</p>
  <p class="subtitle" id="uptime-label" style="color:#00ffcc; font-size:0.8em;"></p>

  <div class="container">
    <!-- GAUGES -->
    <div class="gauges-row">
      <div class="gauge-card">
        <p class="gauge-title s1-color">⬤ Sensor 1</p>
        <div class="gauge gauge-bg-1">
          <div id="gauge-fill-1" class="gauge-fill" style="height:100%;"></div>
          <div id="gauge-label-1" class="gauge-label">-- °C</div>
        </div>
      </div>
      <div class="gauge-card">
        <p class="gauge-title s2-color">⬤ Sensor 2</p>
        <div class="gauge gauge-bg-2">
          <div id="gauge-fill-2" class="gauge-fill" style="height:100%;"></div>
          <div id="gauge-label-2" class="gauge-label">-- °C</div>
        </div>
      </div>
    </div>

    <!-- STATS -->
    <div class="stats-row">
      <div class="stat-card stat-s1">
        <p>Promedio S1</p><h3 id="avg1">--</h3>
      </div>
      <div class="stat-card stat-s1">
        <p>Máx S1</p><h3 id="max1">--</h3>
      </div>
      <div class="stat-card stat-s1">
        <p>Mín S1</p><h3 id="min1">--</h3>
      </div>
      <div class="stat-card stat-s2">
        <p>Promedio S2</p><h3 id="avg2">--</h3>
      </div>
      <div class="stat-card stat-s2">
        <p>Máx S2</p><h3 id="max2">--</h3>
      </div>
      <div class="stat-card stat-s2">
        <p>Mín S2</p><h3 id="min2">--</h3>
      </div>
    </div>

    <!-- CHART -->
    <div class="chart-container">
      <canvas id="chart"></canvas>
    </div>

    <!-- ALERTA -->
    <div id="alert-banner" style="display:none; background:linear-gradient(135deg,#e74c3c,#c0392b); color:#fff;
         border-radius:10px; padding:14px 18px; margin-bottom:16px; font-weight:600;
         font-size:0.95em; animation: pulse 1s infinite alternate; text-align:left;">
      ⚠️ <span id="alert-msg"></span>
    </div>
    <style>
      @keyframes pulse { from { opacity:1; } to { opacity:0.7; } }
    </style>

    <!-- RELAY -->
    <p class="section-title">Control del Relé</p>
    <div class="control-buttons">
      <button class="btn-on"  onclick="sendRelayCommand('on')">▶ Encender</button>
      <button class="btn-off" onclick="sendRelayCommand('off')">■ Apagar</button>
      <button class="btn-auto"onclick="sendRelayCommand('auto')">⟳ Automático</button>
    </div>
    <div id="relay-status" class="status-badge relay-auto">Control Automático</div>

    <!-- RED DE NODOS -->
    <p class="section-title">🌐 Red de Nodos</p>
    <div id="nodes-container"></div>

    <br>
    <nav>
      <a href="/config">⚙ Configuración</a>
    </nav>
  </div>

  <script>
    const NODE_IPS = '%NODE_IPS%';
    const canvas = document.getElementById('chart');
    const ctx = canvas ? canvas.getContext('2d') : null;
    let chart = null;
    try {
      if(ctx && typeof Chart !== 'undefined') {
        chart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: [],
        datasets: [
          {
            label: 'Sensor 1',
            data: [],
            borderColor: '#00aaff',
            backgroundColor: 'rgba(0,170,255,0.15)',
            fill: true,
            tension: 0.4,
            pointRadius: 2,
            borderWidth: 2
          },
          {
            label: 'Sensor 2',
            data: [],
            borderColor: '#ff6b6b',
            backgroundColor: 'rgba(255,107,107,0.15)',
            fill: true,
            tension: 0.4,
            pointRadius: 2,
            borderWidth: 2
          }
        ]
      },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        scales: {
          x: {
            ticks: { color: '#7a8a9a', maxTicksLimit: 8 },
            grid: { color: 'rgba(255,255,255,0.05)' }
          },
          y: {
            title: { display: true, text: 'Temperatura (°C)', color: '#7a8a9a' },
            ticks: { color: '#7a8a9a' },
            grid: { color: 'rgba(255,255,255,0.05)' }
          }
        },
        plugins: {
          legend: {
            labels: { color: '#e0e6ed', font: { family: 'Inter', size: 12 } }
          }
        }
      }
    });
      }
    } catch(e) { console.warn('Chart.js no se cargó (sin internet?)', e); }

    function updateGauge(fillId, labelId, temp) {
      const label = document.getElementById(labelId);
      const fill  = document.getElementById(fillId);
      label.innerText = temp.toFixed(1) + ' °C';
      let norm = (temp + 10) / 120;
      norm = Math.max(0, Math.min(1, norm));
      fill.style.height = (1 - norm) * 100 + '%';
    }

    async function fetchData() {
      try {
        const res  = await fetch('/data.json');
        const data = await res.json();
        if (data.length === 0) return;

        const labels = data.map(d => {
          const dt = new Date(d.time * 1000);
          return dt.toLocaleTimeString();
        });

        if (chart) {
          chart.data.labels                   = labels;
          chart.data.datasets[0].data         = data.map(d => d.t1);
          chart.data.datasets[1].data         = data.map(d => d.t2);
          chart.update();
        }

        const last  = data[data.length - 1];
        const t1arr = data.map(d => d.t1).filter(v => v > -100);
        const t2arr = data.map(d => d.t2).filter(v => v > -100);

        updateGauge('gauge-fill-1', 'gauge-label-1', last.t1 > -100 ? last.t1 : 0);
        updateGauge('gauge-fill-2', 'gauge-label-2', last.t2 > -100 ? last.t2 : 0);

        if(t1arr.length > 0) {
          document.getElementById('avg1').innerText = (t1arr.reduce((a,b)=>a+b,0)/t1arr.length).toFixed(1)+' °C';
          document.getElementById('max1').innerText = Math.max(...t1arr).toFixed(1)+' °C';
          document.getElementById('min1').innerText = Math.min(...t1arr).toFixed(1)+' °C';
        }
        if(t2arr.length > 0) {
          document.getElementById('avg2').innerText = (t2arr.reduce((a,b)=>a+b,0)/t2arr.length).toFixed(1)+' °C';
          document.getElementById('max2').innerText = Math.max(...t2arr).toFixed(1)+' °C';
          document.getElementById('min2').innerText = Math.min(...t2arr).toFixed(1)+' °C';
        }

        document.getElementById('ts-label').innerText =
          'Ultima actualizacion: ' + (last ? new Date(last.time * 1000).toLocaleTimeString() : 'Buscando sensores...');
      } catch(e) { 
        document.getElementById('ts-label').innerText = 'JS Err: ' + e;
        console.warn('fetchData error', e); 
      }
    }

    async function fetchStatus() {
      try {
        const res    = await fetch('/status');
        const status = await res.json();

        // Uptime
        const sec  = status.uptime || 0;
        const hh   = String(Math.floor(sec / 3600)).padStart(2,'0');
        const mm   = String(Math.floor((sec % 3600) / 60)).padStart(2,'0');
        const ss   = String(sec % 60).padStart(2,'0');
        document.getElementById('uptime-label').innerText =
          '⏱ Uptime: ' + hh + ':' + mm + ':' + ss +
          '  |  💾 EEPROM writes: ' + (status.eepromWrites || 0);

        // Relay
        const el = document.getElementById('relay-status');
        if (status.manualControl) {
          el.className = 'status-badge ' + (status.relayState ? 'relay-on' : 'relay-off');
          el.innerText = 'Manual: ' + (status.relayState ? 'ENCENDIDO' : 'APAGADO');
        } else {
          el.className = 'status-badge relay-auto';
          el.innerText = '⟳ Control Automatico';
        }

        // Alertas
        const banner = document.getElementById('alert-banner');
        const msg    = document.getElementById('alert-msg');
        if (status.alertActive) {
          banner.style.display = 'block';
          msg.innerText = status.alertMsg;
        } else {
          banner.style.display = 'none';
        }
      } catch(e) {
        document.getElementById('uptime-label').innerText = 'Err Estado: ' + e;
      }
    }

    async function sendRelayCommand(cmd) {
      await fetch('/relay-control?cmd=' + cmd);
    }

    async function fetchNode(ip) {
      if (!ip) return;
      try {
        // Fetch simple sin AbortSignal moderno
        const res = await fetch('http://' + ip + '/node-info');
        const d   = await res.json();
        const s   = d.data || {};
        
        let c = document.getElementById('n_' + ip);
        if (!c) {
          c = document.createElement('div');
          c.id = 'n_' + ip;
          c.style.marginBottom = '12px';
          document.getElementById('nodes-container').appendChild(c);
        }
        
        let h = '<div style="background:rgba(255,255,255,0.04); border-radius:10px; padding:12px; border:1px solid rgba(255,255,255,0.08);">';
        h += '<div style="font-size:0.8em; color:#00aaff; font-weight:600; margin-bottom:8px;">► NODO: ' + d.node_id.toUpperCase() + ' [' + d.ip + ']</div>';

        if (d.node_type === 'motor_monitor') {
          h += '<div style="display:flex; gap:8px; flex-wrap:wrap; margin-bottom:10px;">';
          h += '<div class="stat-card" style="flex:1; border-top:3px solid #f39c12;"><p>Corriente</p><h3>'+(s.current_amp||0).toFixed(2)+'A</h3></div>';
          h += '<div class="stat-card" style="flex:1; border-top:3px solid #9b59b6;"><p>Ciclos</p><h3>'+(s.total_cycles||0)+'</h3></div>';
          h += '</div>';
          h += '<div style="font-size:0.82em; padding:8px; background:rgba(0,0,0,0.2); border-radius:6px; margin-bottom:6px;">' + (s.diagnostics||'--') + '</div>';
          if (s.last_cut_temp1_c > 0) {
            h += '<div style="font-size:0.75em; color:#00ffcc;">🌡 Temp al corte S1: ' + s.last_cut_temp1_c.toFixed(1) + '°C';
            if (s.last_cut_temp2_c > 0) h += ' | S2: ' + s.last_cut_temp2_c.toFixed(1) + '°C';
            h += '</div>';
          }
          let stColor = s.protection_tripped ? '#e74c3c' : (s.motor_on ? '#2ecc71' : '#7a8a9a');
          let stText  = s.protection_tripped ? '⚠️ PROTECCIÓN ACTIVA' : (s.motor_on ? '⚡ MOTOR ON' : 'Standby');
          h += '<div style="font-size:0.8em; font-weight:700; color:' + stColor + '; margin-top:6px; padding:4px 8px; background:rgba(0,0,0,0.2); border-radius:6px;">Estado: ' + stText + '</div>';
          if (s.is_stalled) h += '<div style="font-size:0.75em; color:#e74c3c; font-weight:600; margin-top:4px;">🔴 MOTOR CLAVADO - Corriente cortada</div>';
        } 
        else if (d.node_type === 'thermostat') {
          h += '<div style="display:flex; gap:8px; flex-wrap:wrap; margin-bottom:10px;">';
          h += '<div class="stat-card" style="flex:1; border-top:3px solid #00aaff;"><p>Temp S1</p><h3>'+(s.temp1_c||0).toFixed(1)+'°C</h3></div>';
          h += '<div class="stat-card" style="flex:1; border-top:3px solid #ff6b6b;"><p>Temp S2</p><h3>'+(s.temp2_c||0).toFixed(1)+'°C</h3></div>';
          h += '</div>';
          let rSt = s.relay_on ? '<span style="color:#2ecc71">ON</span>' : '<span style="color:#e74c3c">OFF</span>';
          h += '<div style="font-size:0.8em; margin-bottom:4px;">Relay: ' + rSt + ' | Objetivo: ' + (s.target_temp_c||0) + '°C</div>';
          if (s.alert_active) h += '<div style="font-size:0.8em; color:#e74c3c;">⚠️ ' + s.alert_msg + '</div>';
        }
        else {
          h += '<div style="font-size:0.8em; color:#7a8a9a;">Información no disponible</div>';
        }
        
        h += '</div>';
        c.innerHTML = h;
      } catch(e) {
        let c = document.getElementById('n_' + ip);
        if (c) c.innerHTML = '<div style="background:rgba(255,255,255,0.04); border-radius:10px; padding:12px; border:1px solid rgba(255,255,255,0.08);"><div style="font-size:0.8em; color:#e74c3c; font-weight:600;">► NODO [' + ip + '] Offline</div></div>';
      }
    }

    function fetchAllNodes() {
      if(!NODE_IPS) return;
      const ips = NODE_IPS.split(',');
      ips.forEach(ip => {
        ip = ip.trim();
        if(ip) fetchNode(ip);
      });
    }

    setInterval(fetchData, 2000);
    setInterval(fetchStatus, 2000);
    setInterval(fetchAllNodes, 5000);
    fetchData();
    fetchStatus();
    fetchAllNodes();
  </script>
</body>
</html>
)rawliteral";
    request->send_P(200, "text/html", page, [](const String& var) -> String {
      if (var == "NODE_IPS") return String(config.nodeIPs);
      return String();
    });
  });

  // =========================================================
  //  RUTA: /config
  // =========================================================
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <title>Configuración</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;600&display=swap');
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Inter', sans-serif;
      background: linear-gradient(135deg, #0f0f1a, #1a1a2e);
      color: #e0e6ed;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 20px;
    }
    h1 { color: #00aaff; margin-bottom: 20px; font-size: 1.5em; }
    .container {
      background: rgba(255,255,255,0.04);
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 16px;
      padding: 24px;
      width: 100%;
      max-width: 420px;
    }
    label { display: block; font-size: 0.82em; color: #7a8a9a; margin-bottom: 4px; }
    input[type="text"], input[type="number"], input[type="password"], select {
      width: 100%;
      padding: 10px 12px;
      margin-bottom: 14px;
      border: 1px solid rgba(255,255,255,0.12);
      border-radius: 8px;
      background: rgba(255,255,255,0.06);
      color: #e0e6ed;
      font-size: 0.95em;
      font-family: 'Inter', sans-serif;
    }
    input::placeholder { color: #7a8a9a; }
    .divider {
      border: none;
      border-top: 1px solid rgba(255,255,255,0.08);
      margin: 16px 0;
    }
    .section-label {
      font-size: 0.72em;
      letter-spacing: 1.5px;
      text-transform: uppercase;
      color: #7a8a9a;
      margin-bottom: 12px;
    }
    button[type="submit"] {
      width: 100%;
      padding: 12px;
      background: linear-gradient(135deg, #00aaff, #1a6fff);
      color: #fff;
      border: none;
      border-radius: 8px;
      font-size: 1em;
      font-weight: 600;
      font-family: 'Inter', sans-serif;
      cursor: pointer;
      transition: opacity 0.2s;
      margin-top: 6px;
    }
    button[type="submit"]:hover { opacity: 0.85; }
    a { color: #00aaff; text-decoration: none; font-size: 0.9em; }
    a:hover { color: #00ffcc; }
    .back { display: block; text-align: center; margin-top: 16px; }
  </style>
</head>
<body>
  <h1>⚙ Configuración</h1>
  <div class="container">
    <form action="/save-config" method="post">
      <p class="section-label">Red WiFi</p>
      <label>SSID</label>
      <input type="text" name="ssid" value=")rawliteral" + String(config.wifiSSID) + R"rawliteral(">
      <label>Contraseña</label>
      <input type="password" name="pass" value=")rawliteral" + String(config.wifiPASS) + R"rawliteral(">

      <hr class="divider">
      <p class="section-label">&#9889; Monitor Sentinel Pro v6.0</p>
      <label>IP del Sentinel (para monitoreo cruzado)</label>
      <input type="text" name="nodeIPs" placeholder="192.168.1.81" value=")rawliteral" + String(config.nodeIPs) + R"rawliteral(">

      <hr class="divider">
      <p class="section-label">Control de Temperatura</p>

      <label>Temperatura objetivo (°C)</label>
      <input type="number" step="0.1" name="targettemp" value=")rawliteral" + String(config.targetTemp) + R"rawliteral(">
      <label>Sensor que controla el relé</label>
      <select name="sensorControl">
        <option value="0" )rawliteral" + (config.sensorControl == 0 ? "selected" : "") + R"rawliteral(>Sensor 1</option>
        <option value="1" )rawliteral" + (config.sensorControl == 1 ? "selected" : "") + R"rawliteral(>Sensor 2</option>
        <option value="2" )rawliteral" + (config.sensorControl == 2 ? "selected" : "") + R"rawliteral(>Promedio de ambos</option>
      </select>

      <hr class="divider">
      <details>
      <summary style="cursor:pointer;padding:10px;background:rgba(0,170,255,0.08);border-radius:8px;font-weight:700;font-size:0.82em;color:#00aaff;letter-spacing:1px;user-select:none">&#9881; AJUSTE AVANZADO &mdash; Alertas &amp; Calibraci&oacute;n (expandir)</summary>
      <div style="padding-top:12px">

      <p class="section-label">Intervalo de Muestreo</p>
      <label>Intervalo de muestreo (ms) — default 10000</label>
      <input type="number" name="interval" value=")rawliteral" + String(config.sampleInterval) + R"rawliteral(">

      <hr class="divider">
      <p class="section-label">Alertas de Temperatura</p>
      <label>Activar alertas</label>
      <select name="alertsEnabled">
        <option value="1" )rawliteral" + (config.alertsEnabled ? "selected" : "") + R"rawliteral(>Activadas</option>
        <option value="0" )rawliteral" + (!config.alertsEnabled ? "selected" : "") + R"rawliteral(>Desactivadas</option>
      </select>
      <label>Temperatura m&aacute;xima de alerta (&deg;C)</label>
      <input type="number" step="0.1" name="alertMax" value=")rawliteral" + String(config.alertMaxTemp) + R"rawliteral(">
      <label>Temperatura m&iacute;nima de alerta (&deg;C)</label>
      <input type="number" step="0.1" name="alertMin" value=")rawliteral" + String(config.alertMinTemp) + R"rawliteral(">

      <hr class="divider">
      <p class="section-label">Calibraci&oacute;n de Sensores DS18B20</p>
      <label>Offset Sensor 1 (&deg;C) — corrección de error del sensor</label>
      <input type="number" step="0.1" name="offset1" value=")rawliteral" + String(config.offsetTempDS18B20_1) + R"rawliteral(">
      <label>Offset Sensor 2 (&deg;C) — corrección de error del sensor</label>
      <input type="number" step="0.1" name="offset2" value=")rawliteral" + String(config.offsetTempDS18B20_2) + R"rawliteral(">

      </div></details>

      <button type="submit">Guardar y Reiniciar</button>
    </form>

    <hr class="divider">
    <p class="section-label">Actualización de Firmware</p>
    <a href="/ota" style="display:block; width:100%; padding:12px; background:linear-gradient(135deg,#f39c12,#e67e22);
       color:#fff; border:none; border-radius:8px; font-size:1em; font-weight:600;
       font-family:'Inter',sans-serif; cursor:pointer; text-align:center;
       text-decoration:none; box-sizing:border-box;">
      📦 Actualizar Firmware (OTA)
    </a>

    <a class="back" href="/">← Volver al Dashboard</a>
  </div>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  // =========================================================
  //  RUTA: /save-config
  // =========================================================
  server.on("/save-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("ssid", true))
      strcpy(config.wifiSSID, request->getParam("ssid", true)->value().c_str());
    if (request->hasParam("pass", true))
      strcpy(config.wifiPASS, request->getParam("pass", true)->value().c_str());
    if (request->hasParam("interval", true))
      config.sampleInterval = request->getParam("interval", true)->value().toInt();
    if (request->hasParam("targettemp", true))
      config.targetTemp = request->getParam("targettemp", true)->value().toDouble();
    if (request->hasParam("offset1", true))
      config.offsetTempDS18B20_1 = request->getParam("offset1", true)->value().toDouble();
    if (request->hasParam("offset2", true))
      config.offsetTempDS18B20_2 = request->getParam("offset2", true)->value().toDouble();
    if (request->hasParam("sensorControl", true))
      config.sensorControl = (uint8_t)request->getParam("sensorControl", true)->value().toInt();
    if (request->hasParam("alertMax", true))
      config.alertMaxTemp = request->getParam("alertMax", true)->value().toDouble();
    if (request->hasParam("alertMin", true))
      config.alertMinTemp = request->getParam("alertMin", true)->value().toDouble();
    if (request->hasParam("alertsEnabled", true))
      config.alertsEnabled = (request->getParam("alertsEnabled", true)->value().toInt() == 1);
    if (request->hasParam("nodeIPs", true)) {
      String pip = request->getParam("nodeIPs", true)->value();
      pip.trim();
      strncpy(config.nodeIPs, pip.c_str(), 39);
      config.nodeIPs[39] = '\0';
    }

    saveConfig();
    request->redirect("/reboot-wait");
    rebootNeeded = true;
  });

  // =========================================================
  //  RUTA: /relay-control
  // =========================================================
  server.on("/relay-control", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("cmd")) {
      request->send(400, "text/plain", "Falta parametro cmd");
      return;
    }
    String cmd = request->getParam("cmd")->value();
    if      (cmd == "on")   { manualControl = true;  relayState = true;  }
    else if (cmd == "off")  { manualControl = true;  relayState = false; }
    else if (cmd == "auto") { manualControl = false; }
    request->send(200, "text/plain", "OK");
  });

  // =========================================================
  //  RUTA: /status
  // =========================================================
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String response = "{";
    response += "\"manualControl\":" + String(manualControl ? "true" : "false") + ",";
    response += "\"relayState\":" + String(relayState ? "true" : "false") + ",";
    response += "\"t1\":" + String(lastTemp1, 1) + ",";
    response += "\"t2\":" + String(lastTemp2, 1) + ",";
    response += "\"alertActive\":" + String(alertActive ? "true" : "false") + ",";
    response += "\"alertMsg\":\"" + alertMessage + "\",";
    response += "\"eepromWrites\":" + String(eepromWrites) + ",";
    response += "\"uptime\":" + String(millis() / 1000);
    response += "}";
    
    request->send(200, "application/json", response);
  });

  // =========================================================
  //  RUTA: /data.json
  // =========================================================
  server.on("/data.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    String response = "[";
    bool first = true;
    for (int i = 0; i < MAX_HISTORY; i++) {
      int idx = (historyIndex + i) % MAX_HISTORY;
      if (timeHistory[idx] != 0) {
        if (!first) response += ",";
        response += "{\"time\":";
        response += String(timeHistory[idx]);
        response += ",\"t1\":";
        response += String(tempHistory1[idx], 1);
        response += ",\"t2\":";
        response += String(tempHistory2[idx], 1);
        response += "}";
        first = false;
      }
    }
    response += "]";
    request->send(200, "application/json", response);
  });

  // =========================================================
  //  RUTA: /reboot-wait
  // =========================================================
  server.on("/reboot-wait", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = R"rawliteral(
<!DOCTYPE html><html><head>
  <title>Reiniciando</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin-top: 60px;
           background: #0f0f1a; color: #e0e6ed; }
    h1 { color: #00aaff; } p { font-size: 1.1em; margin-top: 12px; }
  </style>
</head>
<body>
  <h1>✅ Configuración Guardada</h1>
  <p>El dispositivo se está reiniciando...</p>
  <p>Esta página se actualizará automáticamente.</p>
  <script>
    function check() {
      fetch('/').then(r => { if(r.ok) window.location.href='/'; }).catch(()=>{});
    }
    setInterval(check, 2000);
  </script>
</body></html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  // =========================================================
  //  RUTA: /ota  (página de actualización de firmware)
  // =========================================================
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <title>Actualizar Firmware</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700&display=swap');
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Inter', sans-serif;
      background: linear-gradient(135deg, #0f0f1a, #1a1a2e);
      color: #e0e6ed;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 30px 20px;
    }
    h1 { font-size: 1.5em; color: #f39c12; margin-bottom: 6px; }
    .sub { color: #7a8a9a; font-size: 0.85em; margin-bottom: 24px; }
    .card {
      background: rgba(255,255,255,0.04);
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 16px;
      padding: 28px;
      width: 100%;
      max-width: 420px;
    }
    .file-area {
      border: 2px dashed rgba(243,156,18,0.4);
      border-radius: 10px;
      padding: 30px;
      text-align: center;
      cursor: pointer;
      transition: border-color 0.2s, background 0.2s;
      margin-bottom: 16px;
      position: relative;
    }
    .file-area:hover { border-color: #f39c12; background: rgba(243,156,18,0.05); }
    .file-area input[type=file] {
      position: absolute; inset: 0; opacity: 0; cursor: pointer; width: 100%;
    }
    .file-icon { font-size: 2.5em; margin-bottom: 8px; }
    .file-name { color: #00ffcc; font-size: 0.9em; margin-top: 8px; font-weight: 600; }
    .info-row {
      display: flex; justify-content: space-between;
      font-size: 0.8em; color: #7a8a9a; margin-bottom: 20px;
    }
    .info-row span { color: #e0e6ed; }

    #upload-btn {
      width: 100%;
      padding: 13px;
      background: linear-gradient(135deg, #f39c12, #e67e22);
      color: #fff;
      border: none;
      border-radius: 8px;
      font-size: 1em;
      font-weight: 700;
      font-family: 'Inter', sans-serif;
      cursor: pointer;
      transition: opacity 0.2s;
    }
    #upload-btn:disabled { opacity: 0.45; cursor: not-allowed; }

    /* Barra de progreso */
    .progress-wrap {
      display: none;
      margin-top: 20px;
    }
    .progress-bar-bg {
      background: rgba(255,255,255,0.08);
      border-radius: 8px;
      height: 14px;
      overflow: hidden;
      margin-bottom: 8px;
    }
    .progress-bar-fill {
      height: 100%;
      width: 0%;
      background: linear-gradient(90deg, #f39c12, #00ffcc);
      border-radius: 8px;
      transition: width 0.3s ease;
    }
    .progress-text { font-size: 0.85em; color: #7a8a9a; text-align: center; }

    /* Resultado */
    .result {
      display: none;
      margin-top: 18px;
      padding: 14px;
      border-radius: 10px;
      font-weight: 600;
      font-size: 0.95em;
      text-align: center;
    }
    .result.ok  { background: rgba(46,204,113,0.15); border: 1px solid #2ecc71; color: #2ecc71; }
    .result.err { background: rgba(231,76,60,0.15);  border: 1px solid #e74c3c; color: #e74c3c; }

    .warning {
      background: rgba(243,156,18,0.1);
      border: 1px solid rgba(243,156,18,0.3);
      border-radius: 8px;
      padding: 10px 14px;
      font-size: 0.8em;
      color: #f39c12;
      margin-bottom: 16px;
    }
    a { color: #00aaff; text-decoration: none; font-size: 0.9em; }
    a:hover { color: #00ffcc; }
    .back { display: block; text-align: center; margin-top: 20px; }
  </style>
</head>
<body>
  <h1>📦 Actualizar Firmware</h1>
  <p class="sub">Subí el archivo .bin generado por Arduino IDE</p>

  <div class="card">
    <div class="warning">
      ⚠️ El dispositivo se reiniciará automáticamente después de una actualización exitosa.
      No cierres esta página durante el proceso.
    </div>

    <div class="file-area" id="drop-area">
      <input type="file" id="fw-file" accept=".bin" onchange="onFileSelected(this)">
      <div class="file-icon">📁</div>
      <div>Hacé clic o arrastrá el archivo <strong>.bin</strong> aquí</div>
      <div class="file-name" id="fname"></div>
    </div>

    <div class="info-row">
      <div>Tamaño: <span id="fsize">—</span></div>
      <div>Espacio libre: <span id="fspace">—</span></div>
    </div>

    <button id="upload-btn" disabled onclick="startUpload()">⬆ Subir Firmware</button>

    <div class="progress-wrap" id="progress-wrap">
      <div class="progress-bar-bg">
        <div class="progress-bar-fill" id="prog-bar"></div>
      </div>
      <div class="progress-text" id="prog-text">Subiendo... 0%</div>
    </div>

    <div class="result" id="result"></div>
  </div>

  <a class="back" href="/config">← Volver a Configuración</a>

  <script>
    // Cargar info de espacio libre del dispositivo
    fetch('/ota-info').then(r => r.json()).then(d => {
      document.getElementById('fspace').innerText = (d.free / 1024).toFixed(1) + ' KB';
    }).catch(() => {
      document.getElementById('fspace').innerText = 'N/D';
    });

    function onFileSelected(input) {
      const file = input.files[0];
      if (!file) return;
      document.getElementById('fname').innerText = file.name;
      document.getElementById('fsize').innerText = (file.size / 1024).toFixed(1) + ' KB';
      document.getElementById('upload-btn').disabled = false;
    }

    async function startUpload() {
      const file = document.getElementById('fw-file').files[0];

      // Validaciones client-side
      if (!file) { showResult(false, 'Seleccioná un archivo primero.'); return; }
      if (!file.name.endsWith('.bin')) { showResult(false, 'El archivo debe ser .bin'); return; }
      if (file.size < 1000) { showResult(false, 'Archivo demasiado pequeño, parece inválido.'); return; }

      // Leer los primeros bytes y verificar magic byte ESP8266 (0xE9)
      const slice = file.slice(0, 4);
      const ab = await slice.arrayBuffer();
      const bytes = new Uint8Array(ab);
      if (bytes[0] !== 0xE9) {
        showResult(false, 'Archivo inválido: no es un binario ESP8266 (magic byte incorrecto: 0x' + bytes[0].toString(16).toUpperCase() + '). Verificá que exportaste el .bin correcto.');
        return;
      }

      // Empezar upload
      document.getElementById('upload-btn').disabled = true;
      document.getElementById('progress-wrap').style.display = 'block';

      const formData = new FormData();
      formData.append('update', file, file.name);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update', true);

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const pct = Math.round(e.loaded * 100 / e.total);
          document.getElementById('prog-bar').style.width = pct + '%';
          document.getElementById('prog-text').innerText = 'Subiendo... ' + pct + '%';
        }
      };

      xhr.onload = () => {
        if (xhr.status === 200 && xhr.responseText === 'OK') {
          document.getElementById('prog-text').innerText = 'Completado! Esperando reinicio...';
          showResult(true, '✅ Firmware actualizado correctamente. Redirigiendo al dashboard...');
          // Polling hasta que el dispositivo vuelva
          setTimeout(() => {
            const poll = setInterval(() => {
              fetch('/').then(r => { if (r.ok) { clearInterval(poll); window.location.href = '/'; }}).catch(() => {});
            }, 2000);
          }, 4000);
        } else {
          showResult(false, '❌ Error al flashear: ' + xhr.responseText);
          document.getElementById('upload-btn').disabled = false;
        }
      };

      xhr.onerror = () => {
        // Si hay error de red probablemente el dispositivo ya se reinició
        document.getElementById('prog-text').innerText = 'Reiniciando dispositivo...';
        showResult(true, '✅ Firmware enviado. Esperando que el dispositivo vuelva...');
        setTimeout(() => {
          const poll = setInterval(() => {
            fetch('/').then(r => { if (r.ok) { clearInterval(poll); window.location.href = '/'; }}).catch(() => {});
          }, 2000);
        }, 3000);
      };

      xhr.send(formData);
    }

    function showResult(ok, msg) {
      const el = document.getElementById('result');
      el.className = 'result ' + (ok ? 'ok' : 'err');
      el.innerText = msg;
      el.style.display = 'block';
    }
  </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  // =========================================================
  //  RUTA: /ota-info  (espacio libre para el JS)
  // =========================================================
  server.on("/ota-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<64> doc;
    doc["free"] = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    serializeJson(doc, *resp);
    request->send(resp);
  });

  // =========================================================
  //  RUTA: /update  (recibe el .bin y flashea)
  // =========================================================
  server.on("/update", HTTP_POST,
    // Respuesta final — se ejecuta cuando terminó el upload completo
    [](AsyncWebServerRequest *request) {
      bool ok = !Update.hasError();
      String msg = ok ? "OK" : Update.getErrorString();
      AsyncWebServerResponse *resp = request->beginResponse(200, "text/plain", msg);
      resp->addHeader("Connection", "close");
      request->send(resp);
      if (ok) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("OTA completado!");
        lcd.setCursor(0, 1); lcd.print("Reiniciando...");
        delay(500);
        ESP.restart();
      }
    },
    // Handler de chunks — se llama por cada trozo del archivo
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
        // Primer chunk: inicializar el update
        Serial.printf("OTA inicio: %s (%u bytes disponibles)\n", filename.c_str(), (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
        uint32_t maxSize = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSize)) {
          Serial.println("OTA Error al iniciar:");
          Update.printError(Serial);
          lcd.clear();
          lcd.setCursor(0, 0); lcd.print("OTA Error!");
          lcd.setCursor(0, 1); lcd.print("Sin espacio?");
        }
      }
      if (!Update.hasError() && len > 0) {
        if (Update.write(data, len) != len) {
          Serial.println("OTA Error al escribir:");
          Update.printError(Serial);
        }
        // Progreso en LCD
        int pct = (Update.progress() * 100) / Update.size();
        lcd.setCursor(0, 0); lcd.print("OTA:            ");
        lcd.setCursor(4, 0); lcd.print(pct); lcd.print("%  ");
        lcd.setCursor(0, 1);
        int filled = pct * 16 / 100;
        for (int i = 0; i < 16; i++) lcd.print(i < filled ? '=' : '-');
      }
      if (final) {
        if (Update.end(true)) {
          Serial.printf("OTA OK: %u bytes\n", index + len);
        } else {
          Serial.println("OTA Error final:");
          Update.printError(Serial);
        }
      }
    }
  );

  // =========================================================
  //  RUTA: /node-info  (API estandarizada para red de nodos)
  // =========================================================
  server.on("/node-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["node_id"]    = "termostato";
    doc["node_type"]  = "thermostat";
    doc["fw_version"] = "5.2";
    doc["uptime_sec"] = millis() / 1000;
    doc["ip"]         = WiFi.localIP().toString();
    JsonObject data   = doc.createNestedObject("data");
    data["temp1_c"]          = lastTemp1;
    data["temp2_c"]          = lastTemp2;
    data["relay_on"]         = relayState;
    data["relay_manual"]     = manualControl;
    data["target_temp_c"]    = config.targetTemp;
    data["alert_active"]     = alertActive;
    data["alert_msg"]        = alertMessage;
    data["eeprom_writes"]    = eepromWrites;
    data["sensor_control"]   = config.sensorControl;     // 0=S1, 1=S2, 2=Promedio
    data["sample_interval_ms"] = config.sampleInterval;  // intervalo de muestreo
    
    String response;
    serializeJson(doc, response);
    AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    request->send(resp);
  });

  server.begin();
}  // fin setup()

// =========================================================
//  LOOP
// =========================================================
void loop() {
  unsigned long now = millis();

  // Reboot si fue solicitado
  if (rebootNeeded) {
    delay(100); ESP.restart();
  }

  // mDNS (solo en modo WiFi normal)
  if (!isAPMode) {
    MDNS.update();
  }

  // Reconexión WiFi
  if (WiFi.status() != WL_CONNECTED && !isAPMode) {
    lcd.setCursor(0, 0); lcd.print("Reconectando... ");
    lcd.setCursor(0, 1); lcd.print("                ");
    WiFi.reconnect();
    delay(5000);
    if (WiFi.status() == WL_CONNECTED) {
      lcd.clear();
      lcd.print("WiFi Conectado");
      lcd.setCursor(0, 1); lcd.print(WiFi.localIP());
      timeClient.begin();
      timeClient.update();
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
  }

  // ----------- Muestreo de sensores -----------
  if (now - lastSample >= config.sampleInterval) {
    sensor1.requestTemperatures();
    sensor2.requestTemperatures();

    double t1 = sensor1.getTempCByIndex(0);
    double t2 = sensor2.getTempCByIndex(0);

    bool t1ok = (t1 != -127.0 && t1 != DEVICE_DISCONNECTED_C);
    bool t2ok = (t2 != -127.0 && t2 != DEVICE_DISCONNECTED_C);

    if (t1ok) lastTemp1 = t1 + config.offsetTempDS18B20_1;
    if (t2ok) lastTemp2 = t2 + config.offsetTempDS18B20_2;

    if (t1ok || t2ok) {
      addHistory(lastTemp1, lastTemp2);
    }

    // --- Chequeo de alertas ---
    alertActive  = false;
    alertMessage = "";
    if (config.alertsEnabled) {
      if (t1ok && lastTemp1 > config.alertMaxTemp) {
        alertActive = true;
        alertMessage += "Sensor 1 sobre maxima: " + String(lastTemp1, 1) + "°C (max " + String(config.alertMaxTemp, 1) + "°C). ";
      }
      if (t2ok && lastTemp2 > config.alertMaxTemp) {
        alertActive = true;
        alertMessage += "Sensor 2 sobre maxima: " + String(lastTemp2, 1) + "°C (max " + String(config.alertMaxTemp, 1) + "°C). ";
      }
      if (t1ok && lastTemp1 < config.alertMinTemp) {
        alertActive = true;
        alertMessage += "Sensor 1 bajo minima: " + String(lastTemp1, 1) + "°C (min " + String(config.alertMinTemp, 1) + "°C). ";
      }
      if (t2ok && lastTemp2 < config.alertMinTemp) {
        alertActive = true;
        alertMessage += "Sensor 2 bajo minima: " + String(lastTemp2, 1) + "°C (min " + String(config.alertMinTemp, 1) + "°C). ";
      }
    }

    // --- Control del relé ---
    if (manualControl) {
      digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
    } else {
      double ctrlTemp;
      if      (config.sensorControl == 0) ctrlTemp = lastTemp1;
      else if (config.sensorControl == 1) ctrlTemp = lastTemp2;
      else                                ctrlTemp = (lastTemp1 + lastTemp2) / 2.0;

      // Histéresis de 0.5°C para evitar oscilación del relé
      if (ctrlTemp < config.targetTemp - 0.5) {
        digitalWrite(RELAY_PIN, HIGH);
        relayState = true;
      } else if (ctrlTemp > config.targetTemp + 0.5) {
        digitalWrite(RELAY_PIN, LOW);
        relayState = false;
      }
    }

    lastSample = now;
  }

  // ----------- LCD: ambas temperaturas en línea 0, IP en línea 1 -----------
  // Línea 0: "T1:XX.X T2:XX.X" (exactamente 16 chars)
  lcd.setCursor(0, 0);
  lcd.print("T1:");
  if (lastTemp1 != DEVICE_DISCONNECTED_C) {
    // Formateamos con 1 decimal para caber en pantalla
    char buf1[6];
    dtostrf(lastTemp1, 4, 1, buf1);
    lcd.print(buf1);
  } else {
    lcd.print(" ERR");
  }
  lcd.print(" T2:");
  if (lastTemp2 != DEVICE_DISCONNECTED_C) {
    char buf2[6];
    dtostrf(lastTemp2, 4, 1, buf2);
    lcd.print(buf2);
  } else {
    lcd.print(" ERR");
  }

  // Línea 1: IP completa (o Modo AP)
  lcd.setCursor(0, 1);
  if (isAPMode) {
    lcd.print("Modo AP         ");
  } else {
    String ip = WiFi.localIP().toString();
    lcd.print(ip);
    for (int s = ip.length(); s < 16; s++) lcd.print(' ');
  }
}

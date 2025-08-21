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

// ------------------- PINES -------------------
const int DS18B20_PIN = D4; 
const int RELAY_PIN = D3;   

// ------------------- Configuración inicial -------------------
struct ConfigStruct {
  char wifiSSID[32];
  char wifiPASS[32];
  unsigned long sampleInterval;
  double targetTemp;
  double offsetTempDS18B20; 
} config;

// Valores por defecto
void setDefaultConfig() {
  strcpy(config.wifiSSID, "Hacker WiFi 2.4Ghz");
  strcpy(config.wifiPASS, "15418045");
  config.sampleInterval = 10000;
  config.targetTemp = 60.0;
  config.offsetTempDS18B20 = 0.0;
}

// ------------------- Sensor, LCD y NTP ------------------
OneWire oneWireBus(DS18B20_PIN);
DallasTemperature sensors(&oneWireBus);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -10800, 60000);

// ------------------- Servidor -------------------
AsyncWebServer server(80);
bool isAPMode = false;

// ------------------- Histórico -------------------
#define MAX_HISTORY 100
double tempHistoryDS18B20[MAX_HISTORY];
unsigned long timeHistory[MAX_HISTORY];
int historyIndex = 0;

// ------------------- Variables -------------------
unsigned long lastSample = 0;
bool rebootNeeded = false; 
bool manualControl = false; // Nueva variable para control manual
bool relayState = false;    // Estado actual del relé (false = apagado, true = encendido)

// ------------------- Funciones de EEPROM -------------------
void saveConfig() {
  EEPROM.begin(sizeof(ConfigStruct));
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
}

void loadConfig() {
  EEPROM.begin(sizeof(ConfigStruct));
  EEPROM.get(0, config);
  EEPROM.end();
  // Validación de datos de EEPROM
  if (strlen(config.wifiSSID) < 5 || strlen(config.wifiPASS) < 8) {
    setDefaultConfig();
  }
}

// ------------------- Funciones de histórico -------------------
void addHistory(double tempDS18B20){
  if(WiFi.status() == WL_CONNECTED){
    timeHistory[historyIndex] = timeClient.getEpochTime();
  } else {
    timeHistory[historyIndex] = millis();
  }
  tempHistoryDS18B20[historyIndex] = tempDS18B20;
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
}

// ------------------- Setup -------------------
void setup() {
  Serial.begin(115200);
  lcd.init(); lcd.backlight();
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // Inicializa el sensor DS18B20
  sensors.begin();

  loadConfig();

  // Mensaje de bienvenida optimizado con versión
  lcd.setCursor(2, 0);
  lcd.print("Byte-E.LAB");
  lcd.setCursor(4, 1); lcd.print("v.5.1");
  delay(3000);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Iniciando...");

  // Conectar a WiFi
  WiFi.begin(config.wifiSSID, config.wifiPASS);
  lcd.setCursor(0, 0); lcd.print("Conectando WiFi");
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 20){
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if(WiFi.status() == WL_CONNECTED){
    lcd.clear();
    lcd.print("WiFi Conectado");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    Serial.println(WiFi.localIP());
    isAPMode = false;
    timeClient.begin();
    timeClient.update();

    if(MDNS.begin("termostato")) {
      Serial.println("mDNS iniciado, puedes acceder en http://termostato.local");
    }

  } else {
    isAPMode = true;
    lcd.clear();
    lcd.print("Fallo WiFi");
    lcd.setCursor(0, 1);
    lcd.print("Modo AP");
    Serial.println("Fallo WiFi. Iniciando en modo AP");
    WiFi.softAP("NodeMCU-Config");
    Serial.print("IP del AP: ");
    Serial.println(WiFi.softAPIP());
  }
  
  sensors.requestTemperatures();
  double tempDS18B20 = sensors.getTempCByIndex(0);
  
  if(tempDS18B20 != -127.0) {
    addHistory(tempDS18B20);
    Serial.printf("Prueba DS18B20: %.2f C\n", tempDS18B20);
  } else {
    lcd.clear();
    lcd.print("Sensor NO FUNCIONA");
    Serial.println("El sensor DS18B20 esta fallando.");
    delay(5000);
  }

  // ------------------- Servidor web -------------------
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Termostato NodeMCU</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body {
      font-family: 'Segoe UI', Arial, sans-serif;
      text-align: center;
      background-color: #2b2f34;
      color: #e0e6ed;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      align-items: center;
    }
    .container {
      background-color: #343a40;
      margin-top: 20px;
      padding: 20px;
      border-radius: 10px;
      box-shadow: 0 4px 10px rgba(0,0,0,0.3);
      width: 90%;
      max-width: 600px;
    }
    h1, h2 {
      color: #00aaff;
    }
    .gauge {
      width: 150px;
      height: 150px;
      margin: 20px auto;
      border-radius: 50%;
      border: 10px solid #4a5158;
      position: relative;
      background: linear-gradient(to top, #3498db 0%, #3498db 50%, #e74c3c 100%);
      overflow: hidden;
    }
    .gauge-fill {
      position: absolute;
      bottom: 0;
      left: 0;
      width: 100%;
      background-color: #343a40;
      transition: height 0.5s ease-out;
    }
    .gauge-label {
      position: absolute;
      top: 50%;
      left: 50%;
      transform: translate(-50%, -50%);
      font-size: 2em;
      font-weight: bold;
      color: #ffffff;
      text-shadow: 1px 1px 2px rgba(0,0,0,0.5);
    }
    .data-stats {
      display: flex;
      justify-content: space-around;
      width: 100%;
      margin-top: 10px;
    }
    .data-stat {
      background-color: #4a5158;
      padding: 10px;
      border-radius: 5px;
      width: 30%;
      box-shadow: 0 2px 5px rgba(0,0,0,0.2);
    }
    .data-stat p {
      margin: 0;
      font-size: 0.9em;
      color: #b8c0c9;
    }
    .data-stat h3 {
      margin: 5px 0 0 0;
      font-size: 1.2em;
      color: #ffffff;
    }
    .chart-container {
      position: relative;
      height: 250px;
      width: 100%;
      margin-top: 20px;
    }
    canvas {
      background: #4a5158;
      border-radius: 8px;
      box-shadow: inset 0 0 5px rgba(0,0,0,0.2);
      max-height: 100%;
    }
    .control-buttons {
      display: flex;
      justify-content: center;
      gap: 10px;
      margin-top: 20px;
      flex-wrap: wrap;
    }
    .control-buttons button {
      padding: 10px 20px;
      border: none;
      border-radius: 5px;
      cursor: pointer;
      font-size: 1em;
      font-weight: bold;
      transition: background-color 0.3s;
    }
    .control-buttons .on { background-color: #2ecc71; color: white; }
    .control-buttons .off { background-color: #e74c3c; color: white; }
    .control-buttons .auto { background-color: #3498db; color: white; }
    .control-buttons button:hover { opacity: 0.8; }
    .status-text {
        margin-top: 10px;
        font-size: 1.1em;
        font-weight: bold;
    }
    .config-link {
        margin-top: 10px;
        color: #00aaff;
        text-decoration: none;
        font-weight: bold;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Termostato NodeMCU</h1>
    <div class="gauge">
      <div id="gauge-fill" class="gauge-fill" style="height: 100%;"></div>
      <div id="gauge-label" class="gauge-label">0 C</div>
    </div>
    <div class="data-stats">
      <div class="data-stat">
        <p>Promedio</p>
        <h3 id="avg">0 C</h3>
      </div>
      <div class="data-stat">
        <p>M&aacute;ximo</p>
        <h3 id="max">0 C</h3>
      </div>
      <div class="data-stat">
        <p>M&iacute;nimo</p>
        <h3 id="min">0 C</h3>
      </div>
    </div>
    <div class="chart-container">
      <canvas id="chart"></canvas>
    </div>

    <hr>
    
    <div class="control-buttons">
      <button class="on" onclick="sendRelayCommand('on')">Encender Rel&eacute;</button>
      <button class="off" onclick="sendRelayCommand('off')">Apagar Rel&eacute;</button>
      <button class="auto" onclick="sendRelayCommand('auto')">Control Autom&aacute;tico</button>
    </div>
    <p class="status-text" id="relay-status">Estado del rel&eacute;: Cargando...</p>
    
    <a href="/config" class="config-link">Ir a configuraci&oacute;n</a>
  </div>
  
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <script>
    const ctx = document.getElementById('chart').getContext('2d');
    const chart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: [],
        datasets: [{
          label: 'Temp DS18B20',
          data: [],
          borderColor: '#00aaff',
          backgroundColor: 'rgba(0, 170, 255, 0.2)',
          fill: true,
          tension: 0.3
        }]
      },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          x: {
            title: {
              display: true,
              text: 'Tiempo',
              color: '#b8c0c9'
            },
            ticks: { color: '#b8c0c9' },
            grid: { color: '#4a5158' }
          },
          y: {
            title: {
              display: true,
              text: 'Temperatura (C)',
              color: '#b8c0c9'
            },
            ticks: { color: '#b8c0c9' },
            grid: { color: '#4a5158' }
          }
        },
        plugins: {
          legend: {
            display: true
          }
        }
      }
    });

    function updateDashboard(tempDS18B20, avg, max, min) {
      document.getElementById('gauge-label').innerText = tempDS18B20.toFixed(2) + ' C';
      document.getElementById('avg').innerText = avg.toFixed(2) + ' C';
      document.getElementById('max').innerText = max.toFixed(2) + ' C';
      document.getElementById('min').innerText = min.toFixed(2) + ' C';
      let tempNormalized = (tempDS18B20 + 50) / (150);
      tempNormalized = Math.max(0, Math.min(1, tempNormalized));
      document.getElementById('gauge-fill').style.height = (1 - tempNormalized) * 100 + '%';
      
      const gauge = document.getElementById('gauge-fill');
      if (tempDS18B20 < 0) {
        gauge.style.backgroundColor = '#3498db';
      } else if (tempDS18B20 >= 0 && tempDS18B20 < 60) {
        gauge.style.backgroundColor = '#2ecc71';
      } else if (tempDS18B20 >= 60 && tempDS18B20 < 100) {
        gauge.style.backgroundColor = '#f1c40f';
      } else {
        gauge.style.backgroundColor = '#e74c3c';
      }
    }

    async function fetchData() {
      const res = await fetch('/data.json');
      const data = await res.json();
      
      if (data.length > 0) {
        chart.data.labels = data.map(d => {
          const date = new Date(d.time * 1000);
          return date.toLocaleTimeString();
        });
        chart.data.datasets[0].data = data.map(d => d.tempDS18B20);
        chart.update();
        
        let lastDS18B20 = data[data.length - 1].tempDS18B20;
        let avg = data.reduce((a, b) => a + b.tempDS18B20, 0) / data.length;
        let max = Math.max(...data.map(d => d.tempDS18B20));
        let min = Math.min(...data.map(d => d.tempDS18B20));
        updateDashboard(lastDS18B20, avg, max, min);
      }
    }

    async function updateRelayStatus() {
        const res = await fetch('/status');
        const status = await res.json();
        const statusElement = document.getElementById('relay-status');
        if (status.manualControl) {
            statusElement.innerText = "Estado del relé: Modo manual (" + (status.relayState ? "ENCENDIDO" : "APAGADO") + ")";
            statusElement.style.color = status.relayState ? "#2ecc71" : "#e74c3c";
        } else {
            statusElement.innerText = "Estado del relé: Control Automático";
            statusElement.style.color = "#3498db";
        }
    }

    async function sendRelayCommand(command) {
        const res = await fetch('/relay-control?cmd=' + command);
        if (res.ok) {
            console.log("Comando " + command + " enviado con &eacute;xito.");
            updateRelayStatus();
        }
    }
    
    setInterval(fetchData, 1000);
    setInterval(updateRelayStatus, 2000);
    fetchData();
    updateRelayStatus();
  </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", page);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Configuraci&oacute;n</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: 'Segoe UI', Arial, sans-serif;
      text-align: center;
      background-color: #2b2f34;
      color: #e0e6ed;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      align-items: center;
    }
    .container {
      background-color: #343a40;
      margin-top: 20px;
      padding: 20px;
      border-radius: 10px;
      box-shadow: 0 4px 10px rgba(0,0,0,0.3);
      width: 90%;
      max-width: 400px;
    }
    h1 {
      color: #00aaff;
    }
    input[type="text"], input[type="number"], input[type="password"] {
      width: 90%;
      padding: 10px;
      margin: 8px 0;
      box-sizing: border-box;
      border: 1px solid #555;
      border-radius: 4px;
      background-color: #4a5158;
      color: #e0e6ed;
    }
    input::placeholder {
      color: #b8c0c9;
    }
    button {
      background-color: #00aaff;
      color: white;
      padding: 14px 20px;
      margin: 10px 0;
      border: none;
      border-radius: 4px;
      cursor: pointer;
      width: 90%;
      font-size: 1em;
      transition: background-color 0.3s ease;
    }
    button:hover {
      background-color: #007bb5;
    }
    .status {
      margin-top: 15px;
      font-size: 1em;
      color: #2ecc71;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Configuraci&oacute;n</h1>
    <form action="/save-config" method="post">
      <p>SSID:</p>
      <input type="text" name="ssid" value=")rawliteral" + String(config.wifiSSID) + R"rawliteral(">
      <p>Contrase&ntilde;a:</p>
      <input type="password" name="pass" value=")rawliteral" + String(config.wifiPASS) + R"rawliteral(">
      <p>Intervalo de muestreo (ms):</p>
      <input type="number" name="interval" value=")rawliteral" + String(config.sampleInterval) + R"rawliteral(">
      <p>Temperatura objetivo (C):</p>
      <input type="number" step="0.1" name="targettemp" value=")rawliteral" + String(config.targetTemp) + R"rawliteral(">
      <p>Offset Temp DS18B20 (C):</p>
      <input type="number" step="0.1" name="offsetTempDS18B20" value=")rawliteral" + String(config.offsetTempDS18B20) + R"rawliteral(">
      <button type="submit">Guardar y Reiniciar</button>
    </form>
  </div>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  server.on("/save-config", HTTP_POST, [](AsyncWebServerRequest *request){
    if(request->hasParam("ssid", true)) {
      strcpy(config.wifiSSID, request->getParam("ssid", true)->value().c_str());
    }
    if(request->hasParam("pass", true)) {
      strcpy(config.wifiPASS, request->getParam("pass", true)->value().c_str());
    }
    if(request->hasParam("interval", true)) {
      config.sampleInterval = request->getParam("interval", true)->value().toInt();
    }
    if(request->hasParam("targettemp", true)) {
      config.targetTemp = request->getParam("targettemp", true)->value().toDouble();
    }
    if(request->hasParam("offsetTempDS18B20", true)) {
      config.offsetTempDS18B20 = request->getParam("offsetTempDS18B20", true)->value().toDouble();
    }
    saveConfig();
    
    request->redirect("/reboot-wait");

    rebootNeeded = true;
  });

  server.on("/relay-control", HTTP_GET, [](AsyncWebServerRequest *request){
      String cmd = request->getParam("cmd")->value();
      if (cmd == "on") {
          manualControl = true;
          relayState = true;
      } else if (cmd == "off") {
          manualControl = true;
          relayState = false;
      } else if (cmd == "auto") {
          manualControl = false;
      }
      request->send(200, "text/plain", "OK");
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
      StaticJsonDocument<64> doc;
      doc["manualControl"] = manualControl;
      doc["relayState"] = relayState;
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      serializeJson(doc, *response);
      request->send(response);
  });

  server.on("/reboot-wait", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
      <!DOCTYPE html>
      <html>
      <head>
        <title>Reiniciando Dispositivo</title>
        <style>
          body { font-family: sans-serif; text-align: center; margin-top: 50px; background-color: #2b2f34; color: #e0e6ed; }
          h1 { color: #00aaff; }
          p { font-size: 1.2em; }
        </style>
      </head>
      <body>
        <h1>Configuraci&oacute;n Guardada!</h1>
        <p>El dispositivo se est&aacute; reiniciando...</p>
        <p>Por favor, espere. Esta p&aacute;gina se actualizar&aacute; autom&aacute;ticamente cuando el dispositivo est&eacute; en l&iacute;nea.</p>
        <script>
          function checkConnection() {
            fetch('/')
              .then(response => {
                if (response.ok) {
                  console.log("Conexi&oacute;n exitosa. Redirigiendo...");
                  window.location.href = '/';
                }
              })
              .catch(error => {
                console.log("A&uacute;n no hay conexi&oacute;n. Reintentando...");
              });
          }
          setInterval(checkConnection, 2000); 
        </script>
      </body>
      </html>
    )rawliteral";
    request->send(200, "text/html", html);
  });
  
  server.on("/data.json", HTTP_GET, [](AsyncWebServerRequest *request){
    const size_t capacity = JSON_ARRAY_SIZE(MAX_HISTORY) + MAX_HISTORY * JSON_OBJECT_SIZE(2);
    DynamicJsonDocument doc(capacity);
    JsonArray dataArray = doc.to<JsonArray>();

    for(int i=0; i < MAX_HISTORY; i++){
      int idx = (historyIndex + i) % MAX_HISTORY;
      if(timeHistory[idx] != 0){
        JsonObject dataPoint = dataArray.createNestedObject();
        dataPoint["time"] = timeHistory[idx];
        dataPoint["tempDS18B20"] = tempHistoryDS18B20[idx];
      }
    }
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  });

  server.begin();
}

// ------------------- Loop -------------------
void loop() {
  unsigned long now = millis();

  if(rebootNeeded){
    Serial.println("Reiniciando...");
    delay(100); 
    ESP.restart(); 
  }
  
  if (WiFi.status() != WL_CONNECTED && !isAPMode) {
    lcd.setCursor(0, 0);
    lcd.print("Reconectando...");
    lcd.setCursor(0, 1);
    lcd.print("                ");
    WiFi.reconnect();
    delay(5000);
    if (WiFi.status() == WL_CONNECTED) {
      lcd.clear();
      lcd.print("WiFi Conectado");
      lcd.setCursor(0, 1);
      lcd.print(WiFi.localIP());
      timeClient.begin();
      timeClient.update();
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
  }

  if(now - lastSample >= config.sampleInterval){
    sensors.requestTemperatures();
    double tempDS18B20 = sensors.getTempCByIndex(0) + config.offsetTempDS18B20;

    if (tempDS18B20 != -127.0) {
      addHistory(tempDS18B20);
      
      lcd.setCursor(0, 0);
      lcd.print("Temp: ");
      lcd.print(tempDS18B20, 2);
      lcd.print(" C  ");

      lcd.setCursor(0, 1);
      if (isAPMode) {
        lcd.print("Modo AP");
      } else {
        lcd.print(WiFi.localIP());
      }
      
      // Control del relé
      if (manualControl) {
          digitalWrite(RELAY_PIN, relayState);
      } else {
          if (tempDS18B20 < config.targetTemp) {
              digitalWrite(RELAY_PIN, HIGH);
          } else {
              digitalWrite(RELAY_PIN, LOW);
          }
      }
    } else {
      Serial.println("Error al leer un sensor!");
      lcd.setCursor(0, 0);
      lcd.print("Error sensor!");
      lcd.setCursor(0, 1); lcd.print("Verificar pines.");
    }
    lastSample = now;
  }
}
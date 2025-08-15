#include <max6675.h>

#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <MAX6675.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ------------------- PINES -------------------
const int SCK_PIN = 14;  // D5
const int SO_PIN  = 12;  // D6
const int CS_PIN  = 13;  // D7
const int RELAY_PIN = D8;

// ------------------- Configuración inicial -------------------
struct ConfigStruct {
  char wifiSSID[32];
  char wifiPASS[32];
  unsigned long sampleInterval;
  double targetTemp;
  double offsetTemp;
} config;

// Valores por defecto
void setDefaultConfig() {
  strcpy(config.wifiSSID, "NOMBRE_DE_TU_WIFI");
  strcpy(config.wifiPASS, "TU_CONTRASEÑA");
  config.sampleInterval = 10000;
  config.targetTemp = 60.0;
  config.offsetTemp = 0.0;
}

// ------------------- Sensor, LCD y NTP -------------------
MAX6675 termo(SCK_PIN, CS_PIN, SO_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -10800, 60000); // -10800 = -3 horas (Argentina)

// ------------------- Servidor -------------------
AsyncWebServer server(80);
bool isAPMode = false;

// ------------------- Histórico -------------------
#define MAX_HISTORY 1000
double tempHistory[MAX_HISTORY];
unsigned long timeHistory[MAX_HISTORY]; // Ahora guarda el Unix Timestamp
int historyIndex = 0;

// ------------------- Variables -------------------
unsigned long lastSample = 0;

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
void addHistory(double temp){
  if(WiFi.status() == WL_CONNECTED){
    timeHistory[historyIndex] = timeClient.getEpochTime();
  } else {
    timeHistory[historyIndex] = millis();
  }
  tempHistory[historyIndex] = temp;
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
}

// ------------------- Setup -------------------
void setup() {
  Serial.begin(115200);
  lcd.init(); lcd.backlight();
  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW);

  loadConfig();

  // Mensaje de bienvenida optimizado
  lcd.setCursor(2, 0); lcd.print("Byte-E.LAB");
  lcd.setCursor(2, 1); lcd.print("Iniciando...");
  delay(3000); 
  lcd.clear();

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
  } else {
    // Si falla, iniciar en modo AP
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

  // PRUEBA DEL SENSOR
  double temp = termo.readCelsius();
  Serial.printf("Prueba del sensor: %.2f C\n", temp);

  if(isnan(temp) || temp == 1.00){
    lcd.clear();
    lcd.print("Sensor NO FUNCIONA");
    Serial.println("El sensor esta fallando.");
    delay(5000);
  }

  // ------------------- Servidor web -------------------

  // Página principal con gráfico completo
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
    <h1>Temperatura MAX6675</h1>
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
          label: 'Temp C',
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
            display: false
          }
        }
      }
    });

    function updateDashboard(temp, avg, max, min) {
      document.getElementById('gauge-label').innerText = temp.toFixed(2) + ' C';
      document.getElementById('avg').innerText = avg.toFixed(2) + ' C';
      document.getElementById('max').innerText = max.toFixed(2) + ' C';
      document.getElementById('min').innerText = min.toFixed(2) + ' C';

      let tempNormalized = (temp - 0) / (200 - 0);
      tempNormalized = Math.max(0, Math.min(1, tempNormalized));
      document.getElementById('gauge-fill').style.height = (1 - tempNormalized) * 100 + '%';
      
      const gauge = document.getElementById('gauge-fill');
      if (temp < 50) {
        gauge.style.backgroundColor = '#2ecc71';
      } else if (temp >= 50 && temp < 150) {
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
          const date = new Date(d.time * 1000); // Multiplica por 1000 para ms
          return date.toLocaleTimeString();
        });
        chart.data.datasets[0].data = data.map(d => d.temp);
        chart.update();
        
        let last = data[data.length - 1].temp;
        let avg = data.reduce((a, b) => a + b.temp, 0) / data.length;
        let max = Math.max(...data.map(d => d.temp));
        let min = Math.min(...data.map(d => d.temp));
        updateDashboard(last, avg, max, min);
      }
    }
    
    setInterval(fetchData, 1000);
    fetchData();
  </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", page);
  });

  // Página de configuración
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
      <p>Offset de temperatura (C):</p>
      <input type="number" step="0.1" name="offsettemp" value=")rawliteral" + String(config.offsetTemp) + R"rawliteral(">
      <button type="submit">Guardar y Reiniciar</button>
    </form>
  </div>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  // Guardar configuración desde web
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
    if(request->hasParam("offsettemp", true)) {
      config.offsetTemp = request->getParam("offsettemp", true)->value().toDouble();
    }
    saveConfig();
    request->redirect("/");
    delay(1000);
    ESP.reset();
  });

  // JSON del histórico completo
  server.on("/data.json", HTTP_GET, [](AsyncWebServerRequest *request){
    String json="[";
    for(int i=0; i < MAX_HISTORY; i++){
      int idx = (historyIndex + i) % MAX_HISTORY;
      if(timeHistory[idx] != 0){
        json += "{\"time\":"+String(timeHistory[idx])+",\"temp\":"+String(tempHistory[idx])+"},";
      }
    }
    if(json.endsWith(",")) json.remove(json.length()-1);
    json += "]";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Content-Type", "application/json;charset=UTF-8");
    request->send(response);
  });
  
  server.begin();
}

// ------------------- Loop -------------------
void loop() {
  unsigned long now = millis();

  // Re-conexión Wi-Fi automática
  if (WiFi.status() != WL_CONNECTED && !isAPMode) {
    lcd.setCursor(0, 0);
    lcd.print("Reconectando...");
    lcd.setCursor(0, 1);
    lcd.print("                ");
    WiFi.reconnect();
    delay(5000); // Espera 5 segundos antes de volver a verificar
    if (WiFi.status() == WL_CONNECTED) {
      lcd.clear();
      lcd.print("WiFi Conectado");
      lcd.setCursor(0, 1);
      lcd.print(WiFi.localIP());
      timeClient.begin();
      timeClient.update();
    }
    return; // Evita el resto del loop hasta que se conecte
  }

  // Si está conectado, actualiza el cliente NTP
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
  }

  if(now - lastSample >= config.sampleInterval){
    double temp = termo.readCelsius();
    if (!isnan(temp) && temp > -100) {
      temp += config.offsetTemp;
      addHistory(temp);
      Serial.printf("Temp: %.2f C\n", temp);
      
      lcd.setCursor(0, 0); 
      lcd.print("Temp:"); 
      lcd.print(temp, 1); 
      lcd.print("C   ");
      lcd.setCursor(0, 1);
      
      if (isAPMode) {
        lcd.print("Modo AP");
      } else {
        lcd.print(WiFi.localIP());
      }
      
      // Control del relé (sigue funcionando en segundo plano)
      if (temp < config.targetTemp) {
        digitalWrite(RELAY_PIN, HIGH);
      } else {
        digitalWrite(RELAY_PIN, LOW);
      }

    } else {
      Serial.println("Error al leer el sensor!");
      lcd.setCursor(0, 0); lcd.print("Error sensor!");
      lcd.setCursor(0, 1); lcd.print("Verificar pines.");
    }
    lastSample = now;
  }
}
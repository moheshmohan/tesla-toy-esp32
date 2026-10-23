#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>

// Wi-Fi credentials and hostname
typedef const char* LPCSTR;
LPCSTR ssid      = "YOUR_WIFI_SSID";
LPCSTR password  = "YOUR_WIFI_PASSWORD";
LPCSTR hostname  = "akku-tesla";

WebServer server(80);

// HTML page: embedded top-down car controls
const char* htmlPage = R"=====(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Akku Tesla Controls</title>
  <!-- Bootstrap Icons -->
  <link href="https://cdn.jsdelivr.net/npm/bootstrap-icons@1.10.5/font/bootstrap-icons.css" rel="stylesheet">
  <style>
    * { box-sizing:border-box; margin:0; padding:0; }
    body { font-family:sans-serif; background:#121212; color:#fff;
      display:flex; justify-content:center; align-items:center; height:100vh; }
    .container { text-align:center; }
    h1 { font-size:1.8rem; margin-bottom:.5rem; }
    .car { position:relative; width:300px; margin:0 auto; }
    .car svg { width:100%; display:block; }
    .btn { position:absolute; width:40px; height:40px;
      background:rgba(58,134,255,.9); border-radius:50%;
      display:flex; justify-content:center; align-items:center;
      cursor:pointer; transition:transform .2s, background .2s; }
    .btn.active { background:#ffc107; transform:scale(1.1); }
    .btn i { font-size:1.2rem; }
    .btn:hover { transform:scale(1.1); }
    .result { margin-top:1rem; font-size:1rem; min-height:1.2em; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🚗 Akku Tesla</h1>
    <div class="car">
      <!-- Car body -->
      <svg viewBox="0 0 200 400" xmlns="http://www.w3.org/2000/svg">
        <path d="M50,120 Q100,60 150,120 L150,280 Q100,340 50,280 Z" fill="#333" stroke="#444" stroke-width="2"/>
        <rect x="70" y="140" width="60" height="80" rx="10" fill="#555"/>
        <rect x="70" y="200" width="60" height="80" rx="10" fill="#555"/>
        <!-- wheels -->
        <circle cx="60" cy="140" r="15" fill="#111"/>
        <circle cx="140" cy="140" r="15" fill="#111"/>
        <circle cx="60" cy="260" r="15" fill="#111"/>
        <circle cx="140" cy="260" r="15" fill="#111"/>
      </svg>
      <!-- Controls -->
      <div class="btn" id="headlight" data-on="/headlight/on" data-off="/headlight/off" style="top:15%; left:50%; transform:translate(-50%,-50%);">
        <i class="bi bi-lightning-fill"></i>
      </div>
      <div class="btn" id="taillight" data-on="/taillight/on" data-off="/taillight/off" style="top:85%; left:50%; transform:translate(-50%,-50%);">
        <i class="bi bi-lightbulb-off-fill"></i>
      </div>
      <div class="btn" id="left-indicator" data-on="/blinker1/on" data-off="/blinker1/off" style="top:50%; left:15%; transform:translate(-50%,-50%);">
        <i class="bi bi-arrow-left-circle-fill"></i>
      </div>
      <div class="btn" id="right-indicator" data-on="/blinker2/on" data-off="/blinker2/off" style="top:50%; left:85%; transform:translate(-50%,-50%);">
        <i class="bi bi-arrow-right-circle-fill"></i>
      </div>
    </div>
    <div class="result" id="result">Awaiting command...</div>
  </div>
  <script>
    document.querySelectorAll('.btn').forEach(btn => {
      let state = false;
      btn.addEventListener('click', () => {
        const path = state ? btn.dataset.off : btn.dataset.on;
        fetch(path)
          .then(res => res.text())
          .then(txt => {
            document.getElementById('result').textContent = txt;
            state = !state;
            btn.classList.toggle('active', state);
          })
          .catch(_ => document.getElementById('result').textContent = "Error");
      });
    });
  </script>
</body>
</html>)=====";

// GPIO pins
const int PIN_BLINKER1  = D0;
const int PIN_BLINKER2  = D1;
const int PIN_HEADLIGHT = D4;
const int PIN_TAILLIGHT = D3;

// State trackers
bool blinker1On = false;
bool blinker2On = false;
bool headlightOn = false;
bool taillightOn = false;

// Request handlers
void handleRoot()              { server.send(200, "text/html", htmlPage); }
void handleBlinker1On()  { blinker1On = true; server.send(200, "text/plain", "Left Indicator ON"); }
void handleBlinker1Off() { blinker1On = false; digitalWrite(PIN_BLINKER1, LOW); server.send(200, "text/plain", "Left Indicator OFF"); }
void handleBlinker2On()  { blinker2On = true; server.send(200, "text/plain", "Right Indicator ON"); }
void handleBlinker2Off() { blinker2On = false; digitalWrite(PIN_BLINKER2, LOW); server.send(200, "text/plain", "Right Indicator OFF"); }
void handleHeadlightOn() { headlightOn = true; digitalWrite(PIN_HEADLIGHT, HIGH); server.send(200, "text/plain", "Headlight ON"); }
void handleHeadlightOff(){ headlightOn = false; digitalWrite(PIN_HEADLIGHT, LOW); server.send(200, "text/plain", "Headlight OFF"); }
void handleTaillightOn() { taillightOn = true; digitalWrite(PIN_TAILLIGHT, HIGH); server.send(200, "text/plain", "Taillight ON"); }
void handleTaillightOff(){ taillightOn = false; digitalWrite(PIN_TAILLIGHT, LOW); server.send(200, "text/plain", "Taillight OFF"); }

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BLINKER1, OUTPUT);
  pinMode(PIN_BLINKER2, OUTPUT);
  pinMode(PIN_HEADLIGHT, OUTPUT);
  pinMode(PIN_TAILLIGHT, OUTPUT);

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print('.');
  }
  Serial.println("\nConnected, IP: " + WiFi.localIP().toString());

  // mDNS
  WiFi.setHostname(hostname);
  if (!MDNS.begin(hostname)) {
    Serial.println("mDNS init failed");
    while (true) delay(1000);
  }
  MDNS.addService("http", "tcp", 80);

  // Routes
  server.on("/",              handleRoot);
  server.on("/blinker1/on",   handleBlinker1On);
  server.on("/blinker1/off",  handleBlinker1Off);
  server.on("/blinker2/on",   handleBlinker2On);
  server.on("/blinker2/off",  handleBlinker2Off);
  server.on("/headlight/on",  handleHeadlightOn);
  server.on("/headlight/off", handleHeadlightOff);
  server.on("/taillight/on",  handleTaillightOn);
  server.on("/taillight/off", handleTaillightOff);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  // Blink logic
  if (blinker1On || blinker2On) {
    if (blinker1On) digitalWrite(PIN_BLINKER1, HIGH);
    if (blinker2On) digitalWrite(PIN_BLINKER2, HIGH);
    delay(500);
    if (blinker1On) digitalWrite(PIN_BLINKER1, LOW);
    if (blinker2On) digitalWrite(PIN_BLINKER2, LOW);
    delay(500);
  }
}

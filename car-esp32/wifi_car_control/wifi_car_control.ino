#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* hostname = "akku-tesla";

WebServer server(80);

// HTML content for the web page
const char* htmlPage = R"=====(
<!DOCTYPE html>
<html>
<head>
    <title>Akku Tesla</title>
    <script>
        function sendCommand(url) {
            var xhr = new XMLHttpRequest();
            xhr.open("GET", url, true);
            xhr.onreadystatechange = function() {
                if (xhr.readyState == 4 && xhr.status == 200) {
                    document.getElementById('result').innerHTML = xhr.responseText;
                }
            }
            xhr.send();
        }
    </script>
</head>
<body>
    <h1>Hello from Akkus Tesla</h1>
    <p>This is a simple web page served by your Tesla</p>
    <button onclick="sendCommand('/blinker1/on')">Blinker 1 ON</button>
    <button onclick="sendCommand('/blinker1/off')">Blinker 1 OFF</button>
    <button onclick="sendCommand('/blinker2/on')">Blinker 2 ON</button>
    <button onclick="sendCommand('/blinker2/off')">Blinker 2 OFF</button>
    <button onclick="sendCommand('/headlight/on')">Headlight ON</button>
    <button onclick="sendCommand('/headlight/off')">Headlight OFF</button>
    <button onclick="sendCommand('/taillight/on')">Taillight ON</button>
    <button onclick="sendCommand('/taillight/off')">Taillight OFF</button>
    <div id="result">Action result will be displayed here</div>
</body>
</html>
)=====";

int LED_PIN_BLINKER1 = D0; // Adjust to your GPIO pin for blinker1
int LED_PIN_BLINKER2 = D1; // Adjust to your GPIO pin for blinker2
int LED_PIN_HEADLIGHT = D4; // GPIO pin for the headlight
int LED_PIN_TAILLIGHT = D3; // GPIO pin for the taillight
bool blinker1On = false;
bool blinker2On = false;
bool headlightOn = false;
bool taillightOn = false;

void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleBlinker1On() { blinker1On = true; server.send(200, "text/plain", "Blinker 1 is now ON"); }
void handleBlinker1Off() { blinker1On = false; digitalWrite(LED_PIN_BLINKER1, LOW); server.send(200, "text/plain", "Blinker 1 is now OFF"); }
void handleBlinker2On() { blinker2On = true; server.send(200, "text/plain", "Blinker 2 is now ON"); }
void handleBlinker2Off() { blinker2On = false; digitalWrite(LED_PIN_BLINKER2, LOW); server.send(200, "text/plain", "Blinker 2 is now OFF"); }

void handleHeadlightOn() { headlightOn = true; digitalWrite(LED_PIN_HEADLIGHT, HIGH); server.send(200, "text/plain", "Headlight is now ON"); }
void handleHeadlightOff() { headlightOn = false; digitalWrite(LED_PIN_HEADLIGHT, LOW); server.send(200, "text/plain", "Headlight is now OFF"); }
void handleTaillightOn() { taillightOn = true; digitalWrite(LED_PIN_TAILLIGHT, HIGH); server.send(200, "text/plain", "Taillight is now ON"); }
void handleTaillightOff() { taillightOn = false; digitalWrite(LED_PIN_TAILLIGHT, LOW); server.send(200, "text/plain", "Taillight is now OFF"); }

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN_BLINKER1, OUTPUT);
  pinMode(LED_PIN_BLINKER2, OUTPUT);
  pinMode(LED_PIN_HEADLIGHT, OUTPUT);
  pinMode(LED_PIN_TAILLIGHT, OUTPUT);
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected to Wi-Fi.");

    // Print network details
  Serial.print("Hostname: ");
  Serial.println(hostname);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());        // <-- Your device’s IPv4
  Serial.print("Subnet mask: ");
  Serial.println(WiFi.subnetMask());
  Serial.print("Gateway IP: ");
  Serial.println(WiFi.gatewayIP());
  Serial.println();

  WiFi.setHostname(hostname);

  if (!MDNS.begin(hostname)) {
    Serial.println("Error setting up MDNS responder!");
    while(1) { delay(1000); }
  }
  Serial.println("mDNS responder started");
  MDNS.addService("http", "tcp", 80);

  server.on("/", handleRoot);
  server.on("/blinker1/on", handleBlinker1On);
  server.on("/blinker1/off", handleBlinker1Off);
  server.on("/blinker2/on", handleBlinker2On);
  server.on("/blinker2/off", handleBlinker2Off);
  server.on("/headlight/on", handleHeadlightOn);
  server.on("/headlight/off", handleHeadlightOff);
  server.on("/taillight/on", handleTaillightOn);
  server.on("/taillight/off", handleTaillightOff);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  
  if (blinker1On || blinker2On) {
    if (blinker1On) { digitalWrite(LED_PIN_BLINKER1, HIGH); }
    if (blinker2On) { digitalWrite(LED_PIN_BLINKER2, HIGH); }
    delay(500);
    if (blinker1On) { digitalWrite(LED_PIN_BLINKER1, LOW); }
    if (blinker2On) { digitalWrite(LED_PIN_BLINKER2, LOW); }
    delay(500);
  }
}

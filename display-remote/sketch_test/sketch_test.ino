#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ==========================================
// WI-FI & SERVER CONFIGURATION
// ==========================================
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* hostname = "akku-tesla";

IPAddress carIP;
volatile bool carResolved = false;
unsigned long lastResolveRetry = 0;

// ==========================================
// ASYNC NETWORK TASK (runs on the second core)
// ==========================================
// Networking (HTTP commands + mDNS resolve) is handled on core 0 so the
// touch/UI loop on core 1 never stalls waiting on the WiFi stack. Button
// taps just drop a short endpoint string into this queue and return instantly.
struct CarCommand {
  char endpoint[40];
};
QueueHandle_t commandQueue = nullptr;

// ==========================================
// PIN & I2C DEFINITIONS
// ==========================================
#define PCF8574_ADDR      0x38
#define CST826_ADDR       0x15

#define I2C_SDA_PIN       8
#define I2C_SCL_PIN       7

#define LCD_BL_PIN        6

// PCF8574 Output Pin Mappings (0-indexed)
#define PCF_I2S_CTRL      0
#define PCF_RTC_INT       1
#define PCF_CHG_N         2
#define PCF_CAM_PWDN      3
#define PCF_TP_INT        4
#define PCF_LCD_RST       5
#define PCF_SDCS          6

// Touch Coordinates Calibration
#define TOUCH_MIRROR_X    false
#define TOUCH_MIRROR_Y    false
#define TOUCH_SWAP_XY     false

// ==========================================
// LOVYANGFX DISPLAY CLASS CONFIGURATION
// ==========================================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Panel_ST7796  _panel_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;     // ESP32-S3 SPI2 (HSPI)
      cfg.spi_mode = 0;             // SPI Mode 0
      cfg.freq_write = 40000000;    // 40 MHz SPI write speed
      cfg.freq_read  = 16000000;    // 16 MHz SPI read speed
      cfg.pin_sclk = 5;             // SCLK Pin
      cfg.pin_mosi = 1;             // MOSI Pin
      cfg.pin_miso = -1;            // MISO Pin (NC)
      cfg.pin_dc   = 3;             // DC Pin
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 2;     // CS Pin
      cfg.pin_rst          = -1;    // RST (handled via PCF8574)
      cfg.pin_busy         = -1;    // BUSY (NC)
      
      cfg.panel_width      = 320;
      cfg.panel_height     = 480;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = false;
      cfg.rgb_order        = false; // BGR order for ST7796 on this board
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;

      _panel_instance.config(cfg);
      setPanel(&_panel_instance);
    }
  }
};

LGFX lcd;

// ==========================================
// APPLICATION DATA & CONFIGURATION
// ==========================================
uint8_t pcf8574_state = 0xFF;  // Default: all pins HIGH

// Interactive state variables
bool blinker1On = false;
bool blinker2On = false;
bool headlightOn = false;
bool taillightOn = false;

// Blinker animation timer
bool flashState = false;
unsigned long lastFlashTime = 0;

// UI details
bool lastPressed = false;
unsigned long lastTimeUpdate = 0;
int mockMinute = 12;
int mockHour = 21;

// ==========================================
// COLOR PALETTE (RGB565) — modern dark EV theme
// ==========================================
#define COLOR_BG          0x0861  // Deep slate (near-black blue)
#define COLOR_PANEL       0x18E3  // Card background
#define COLOR_PANEL_HI    0x2986  // Raised card / subtle line
#define COLOR_BORDER      0x2A69  // Inactive border / dim element
#define COLOR_TEXT        0xFFFF  // Pure white
#define COLOR_MUTED       0x8C71  // Muted grey text

// Accent colors + their dim "fill" variants (for active button backgrounds)
#define COLOR_CYAN        0x2EBF  // Electric cyan  (headlight)
#define COLOR_CYAN_DIM    0x0A52
#define COLOR_AMBER       0xFD84  // Amber          (blinkers)
#define COLOR_AMBER_DIM   0x4980
#define COLOR_RED         0xF904  // Brake red      (taillight)
#define COLOR_RED_DIM     0x4861
#define COLOR_GREEN       0x368B  // Status OK

// Car body materials
#define COLOR_SILVER      0xCE5B  // Metallic silver body
#define COLOR_SILVER_HI   0xE73C  // Sheen highlight
#define COLOR_ROOF        0xAD55  // Roof panel (slightly darker silver)
#define COLOR_GLASS       0x1906  // Dark glass
#define COLOR_GLASS_HI    0x2DCA  // Glass reflection
#define COLOR_TIRE        0x10A2  // Near-black tire

// Legacy aliases (kept so any older references still compile)
#define COLOR_CARD_OFF    COLOR_PANEL
#define COLOR_CARD_BORDER COLOR_BORDER
#define COLOR_TEXT_ON     COLOR_TEXT
#define COLOR_TEXT_OFF    COLOR_MUTED
#define COLOR_ACTIVE_LIGHT COLOR_AMBER
#define COLOR_ACTIVE_AC   COLOR_CYAN
#define COLOR_ACTIVE_SEC  COLOR_RED
#define COLOR_ACTIVE_PLAY COLOR_GREEN

// ==========================================
// LAYOUT (320 x 480 portrait)
// ==========================================
#define SCREEN_W   320
#define SCREEN_H   480
#define STATUS_H   38                       // top status bar
#define HEADER_Y   38                        // title + connection strip
#define HEADER_H   54
#define CANVAS_X   0                         // car illustration region
#define CANVAS_Y   92
#define CANVAS_W   320
#define CANVAS_H   260
#define BTN_W      138
#define BTN_H      50
#define BTN_GAP    12
#define BTN_X1     16
#define BTN_X2     (BTN_X1 + BTN_W + BTN_GAP)   // 166
#define BTN_ROW1   366
#define BTN_ROW2   (BTN_ROW1 + BTN_H + 10)      // 426

// Off-screen back-buffer for the car canvas. Rendering into a buffer that is
// cleared every frame guarantees clean redraws (no stale headlight beams) and
// is flicker-free. Allocated in PSRAM; falls back to direct draw if unavailable.
LGFX_Sprite carCanvas(&lcd);
bool useCanvasSprite = false;

// ==========================================
// PCF8574 EXPANDER FUNCTIONS
// ==========================================
void writePCF8574(uint8_t val) {
  Wire.beginTransmission(PCF8574_ADDR);
  Wire.write(val);
  Wire.endTransmission();
}

void resetLCD() {
  pcf8574_state &= ~(1 << PCF_LCD_RST);
  writePCF8574(pcf8574_state);
  delay(50);
  
  pcf8574_state |= (1 << PCF_LCD_RST);
  writePCF8574(pcf8574_state);
  delay(120);
}

// ==========================================
// CST826 TOUCH DECODING FUNCTIONS
// ==========================================
bool readTouch(uint16_t &tx, uint16_t &ty) {
  Wire.beginTransmission(CST826_ADDR);
  Wire.write(0x02); // Read from register 0x02
  if (Wire.endTransmission() != 0) {
    return false;
  }
  
  uint8_t bytesReceived = Wire.requestFrom(CST826_ADDR, 5);
  if (bytesReceived == 5) {
    uint8_t buf[5];
    for (int i = 0; i < 5; i++) {
      buf[i] = Wire.read();
    }
    
    uint8_t points = buf[0] & 0x0F;
    if (points > 0 && points <= 2) {
      tx = ((buf[1] & 0x0F) << 8) | buf[2];
      ty = ((buf[3] & 0x0F) << 8) | buf[4];
      return true;
    }
  }
  return false;
}

// ==========================================
// mDNS & HTTP COMMAND FUNCTIONS
// ==========================================
void resolveCarIP() {
  Serial.println("Resolving car IP via mDNS...");
  IPAddress ip = MDNS.queryHost("akku-tesla", 300); // 300ms timeout for faster responsiveness
  if (ip != IPAddress(0, 0, 0, 0)) {
    carIP = ip;
    carResolved = true;
    Serial.print("Car resolved at IP: ");
    Serial.println(carIP);
  } else {
    carResolved = false;
    Serial.println("mDNS query failed, will use direct 'akku-tesla.local'");
  }
}

void sendCarCommand(String endpoint) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String host = "akku-tesla.local";
    if (carResolved) {
      host = carIP.toString();
    }
    String url = "http://" + host + endpoint;
    Serial.print("Sending command to: ");
    Serial.println(url);
    
    http.setTimeout(500); // 500ms timeout for faster responsiveness
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      Serial.println("Response: " + payload);
    } else {
      Serial.print("HTTP GET failed, error: ");
      Serial.println(http.errorToString(httpCode).c_str());
      
      // If cached IP failed, force mDNS re-resolve
      if (carResolved) {
        carResolved = false;
      }
    }
    http.end();
  }
}

// Enqueue a command for the network task. Non-blocking: returns immediately
// so the UI stays responsive even if a previous request is still in flight.
void enqueueCommand(const char* endpoint) {
  if (commandQueue == nullptr) return;
  CarCommand cmd;
  strncpy(cmd.endpoint, endpoint, sizeof(cmd.endpoint) - 1);
  cmd.endpoint[sizeof(cmd.endpoint) - 1] = '\0';
  xQueueSend(commandQueue, &cmd, 0); // don't wait if the queue is full
}

// Runs forever on core 0. Drains the command queue and handles mDNS retries.
void networkTask(void* param) {
  for (;;) {
    // Retry mDNS resolution if the car hasn't been found yet.
    if (!carResolved && millis() - lastResolveRetry >= 10000) {
      resolveCarIP();
      lastResolveRetry = millis();
    }

    // Block up to 100ms waiting for a command, then loop to service mDNS.
    CarCommand cmd;
    if (xQueueReceive(commandQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
      sendCarCommand(String(cmd.endpoint));
    }
  }
}

// ==========================================
// RENDER MODULES
// ==========================================
// Color reflecting overall link health, used by the status icons/text.
uint16_t statusColor() {
  if (WiFi.status() != WL_CONNECTED) return COLOR_RED;
  if (!carResolved) return COLOR_AMBER;
  return COLOR_GREEN;
}

void drawWifiIcon(int x, int y, uint16_t c) {
  lcd.fillCircle(x, y + 6, 2, c);
  lcd.drawArc(x, y + 6, 5, 6, 220, 320, c);
  lcd.drawArc(x, y + 6, 9, 10, 220, 320, c);
}

void drawBatteryIcon(int x, int y, uint16_t c) {
  lcd.drawRoundRect(x, y, 22, 12, 3, COLOR_TEXT);
  lcd.fillRect(x + 22, y + 4, 2, 4, COLOR_TEXT);
  lcd.fillRect(x + 2, y + 2, 16, 8, c); // ~85% charge
}

// --- Top status bar: clock, date, connection + battery icons ---
void drawStatusBar() {
  lcd.fillRect(0, 0, SCREEN_W, STATUS_H, COLOR_BG);
  lcd.drawFastHLine(0, STATUS_H - 1, SCREEN_W, COLOR_PANEL_HI);

  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", mockHour, mockMinute);
  lcd.setTextDatum(ML_DATUM);
  lcd.setTextSize(2);
  lcd.setTextColor(COLOR_TEXT);
  lcd.drawString(timeStr, 12, STATUS_H / 2);

  lcd.setTextSize(1);
  lcd.setTextColor(COLOR_MUTED);
  lcd.drawString("SAT  JUN 6", 76, STATUS_H / 2);

  uint16_t c = statusColor();
  drawWifiIcon(256, STATUS_H / 2 - 6, c);
  drawBatteryIcon(284, STATUS_H / 2 - 6, COLOR_GREEN);
}

// --- Header: product title + live connection status line ---
void drawHeader() {
  lcd.fillRect(0, HEADER_Y, SCREEN_W, HEADER_H, COLOR_BG);

  lcd.setTextDatum(MC_DATUM);
  lcd.setTextSize(2);
  lcd.setTextColor(COLOR_TEXT);
  lcd.drawString("AKKU  TESLA", 160, HEADER_Y + 16);

  const char* msg;
  uint16_t c = statusColor();
  if (WiFi.status() != WL_CONNECTED) {
    msg = "OFFLINE  -  RECONNECTING";
  } else if (!carResolved) {
    msg = "CONNECTED  -  FINDING CAR";
  } else {
    msg = "ONLINE  -  CAR LINKED";
  }

  lcd.setTextSize(1);
  int tw = lcd.textWidth(msg);
  int sx = 160 - tw / 2;
  int sy = HEADER_Y + 40;
  lcd.fillCircle(sx - 8, sy, 3, c);
  lcd.setTextDatum(ML_DATUM);
  lcd.setTextColor(c);
  lcd.drawString(msg, sx, sy);
}

// ==========================================
// CAR ILLUSTRATION (rendered into off-screen canvas)
// ==========================================
// Draws the whole top-down car into target g, centered on (cx, cy). Works for
// both the PSRAM sprite (origin 0,0) and direct-to-lcd fallback (origin offset).
void drawCarInto(LovyanGFX* g, int cx, int cy) {
  const int bw = 116;          // body width
  const int bl = 210;          // body length
  const int left = cx - bw / 2;
  const int top = cy - bl / 2;
  const int bottom = cy + bl / 2;

  // 1. Headlight beams + glow pools (front), drawn first so body sits over base
  if (headlightOn) {
    g->fillTriangle(cx - 32, top, cx - 60, top - 22, cx - 4, top - 22, 0x0631);
    g->fillTriangle(cx + 32, top, cx + 4, top - 22, cx + 60, top - 22, 0x0631);
    for (int s = -1; s <= 1; s += 2) {
      int hx = cx + s * 32;
      g->fillSmoothCircle(hx, top - 1, 15, 0x0A52); // outer halo
      g->fillSmoothCircle(hx, top - 1, 9, 0x1D7C);  // mid cyan
      g->fillSmoothCircle(hx, top - 1, 4, COLOR_TEXT);
    }
  }

  // 2. Taillight glow (rear)
  if (taillightOn) {
    g->fillSmoothRoundRect(cx - 46, bottom - 4, 92, 18, 8, 0x5800); // dim red wash
    g->fillSmoothRoundRect(cx - 40, bottom - 2, 80, 12, 6, COLOR_RED);
  }

  // 3. Tires
  g->fillSmoothRoundRect(left - 7,      top + 22,      14, 34, 6, COLOR_TIRE); // FL
  g->fillSmoothRoundRect(left + bw - 7, top + 22,      14, 34, 6, COLOR_TIRE); // FR
  g->fillSmoothRoundRect(left - 7,      bottom - 56,   14, 34, 6, COLOR_TIRE); // RL
  g->fillSmoothRoundRect(left + bw - 7, bottom - 56,   14, 34, 6, COLOR_TIRE); // RR

  // 4. Side mirrors
  g->fillSmoothRoundRect(left - 6,      top + 60, 8, 12, 3, COLOR_SILVER);
  g->fillSmoothRoundRect(left + bw - 2, top + 60, 8, 12, 3, COLOR_SILVER);

  // 5. Body + sheen highlight
  g->fillSmoothRoundRect(left, top, bw, bl, 38, COLOR_SILVER);
  g->drawRoundRect(left + 3, top + 3, bw - 6, bl - 6, 34, COLOR_SILVER_HI);

  // 6. Glass: windshield, roof panel, rear window
  g->fillSmoothRoundRect(cx - 40, top + 40, 80, 46, 16, COLOR_GLASS);
  g->fillSmoothRoundRect(cx - 38, top + 44, 28, 9, 4, COLOR_GLASS_HI); // reflection
  g->fillSmoothRoundRect(cx - 44, top + 90, 88, 66, 18, COLOR_ROOF);   // roof
  g->fillSmoothRoundRect(cx - 40, top + 160, 80, 30, 14, COLOR_GLASS); // rear window

  // 7. Headlight LED strips (front edge)
  uint16_t hl = headlightOn ? COLOR_TEXT : COLOR_BORDER;
  g->fillSmoothRoundRect(cx - 46, top + 8, 30, 7, 3, hl);
  g->fillSmoothRoundRect(cx + 16, top + 8, 30, 7, 3, hl);
  if (headlightOn) {
    g->fillSmoothRoundRect(cx - 46, top + 14, 30, 2, 1, COLOR_CYAN);
    g->fillSmoothRoundRect(cx + 16, top + 14, 30, 2, 1, COLOR_CYAN);
  }

  // 8. Taillight light bar (rear edge) — full-width Tesla style
  uint16_t tl = taillightOn ? COLOR_RED : COLOR_RED_DIM;
  g->fillSmoothRoundRect(cx - 44, bottom - 16, 88, 8, 4, tl);

  // 9. Blinkers (amber, flashing) at the four corners
  for (int corner = 0; corner < 4; corner++) {
    bool isLeft = (corner == 0 || corner == 2);
    bool isFront = (corner == 0 || corner == 1);
    bool on = isLeft ? blinker1On : blinker2On;
    if (!on || !flashState) continue;
    int bxp = cx + (isLeft ? -50 : 50);
    int byp = isFront ? (top + 14) : (bottom - 14);
    g->fillSmoothCircle(bxp, byp, 8, 0x6240); // amber halo
    g->fillSmoothCircle(bxp, byp, 5, COLOR_AMBER);
    g->fillSmoothCircle(bxp, byp, 2, COLOR_TEXT);
  }
}

// Renders the car canvas (sprite if available, else straight to the LCD).
void renderCarCanvas() {
  if (useCanvasSprite) {
    carCanvas.fillScreen(COLOR_BG);
    drawCarInto(&carCanvas, CANVAS_W / 2, CANVAS_H / 2);
    carCanvas.pushSprite(CANVAS_X, CANVAS_Y);
  } else {
    lcd.fillRect(CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H, COLOR_BG);
    drawCarInto(&lcd, CANVAS_X + CANVAS_W / 2, CANVAS_Y + CANVAS_H / 2);
  }
}

// ==========================================
// CONTROL BUTTONS
// ==========================================
void drawButton(int x, int y, const char* label, bool active,
                uint16_t accent, uint16_t accentDim) {
  uint16_t fill   = active ? accentDim : COLOR_PANEL;
  uint16_t border = active ? accent    : COLOR_BORDER;
  uint16_t txt    = active ? COLOR_TEXT : COLOR_MUTED;

  lcd.fillSmoothRoundRect(x, y, BTN_W, BTN_H, 12, fill);
  lcd.drawRoundRect(x, y, BTN_W, BTN_H, 12, border);
  if (active) lcd.drawRoundRect(x + 1, y + 1, BTN_W - 2, BTN_H - 2, 11, border);

  // Left accent bar
  lcd.fillSmoothRoundRect(x + 10, y + 12, 5, BTN_H - 24, 2, active ? accent : COLOR_BORDER);
  // State dot (top-right)
  lcd.fillSmoothCircle(x + BTN_W - 16, y + 15, 4, active ? accent : COLOR_BORDER);

  lcd.setTextDatum(MC_DATUM);
  lcd.setTextSize(2);
  lcd.setTextColor(txt);
  lcd.drawString(label, x + BTN_W / 2 + 6, y + BTN_H / 2);
}

void drawButtons() {
  drawButton(BTN_X1, BTN_ROW1, "BLINKER L", blinker1On, COLOR_AMBER, COLOR_AMBER_DIM);
  drawButton(BTN_X2, BTN_ROW1, "BLINKER R", blinker2On, COLOR_AMBER, COLOR_AMBER_DIM);
  drawButton(BTN_X1, BTN_ROW2, "HEADLIGHT", headlightOn, COLOR_CYAN, COLOR_CYAN_DIM);
  drawButton(BTN_X2, BTN_ROW2, "TAILLIGHT", taillightOn, COLOR_RED, COLOR_RED_DIM);
}

// ==========================================
// CAR STATE TOGGLERS
// ==========================================
void toggleBlinker1() {
  blinker1On = !blinker1On;
  
  // Optimistic UI update: redraw instantly before network call
  renderCarCanvas();
  drawButtons();
  
  enqueueCommand(blinker1On ? "/blinker1/on" : "/blinker1/off");
}

void toggleBlinker2() {
  blinker2On = !blinker2On;
  
  // Optimistic UI update: redraw instantly before network call
  renderCarCanvas();
  drawButtons();
  
  enqueueCommand(blinker2On ? "/blinker2/on" : "/blinker2/off");
}

void toggleHeadlight() {
  headlightOn = !headlightOn;
  
  // Optimistic UI update: redraw instantly before network call
  renderCarCanvas();
  drawButtons();
  
  enqueueCommand(headlightOn ? "/headlight/on" : "/headlight/off");
}

void toggleTaillight() {
  taillightOn = !taillightOn;
  
  // Optimistic UI update: redraw instantly before network call
  renderCarCanvas();
  drawButtons();
  
  enqueueCommand(taillightOn ? "/taillight/on" : "/taillight/off");
}

// ==========================================
// ARDUINO CORE HOOKS
// ==========================================
void setup() {
  Serial.begin(115200);
  
  // Wire library initialization for I2C (SDA=8, SCL=7)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000); // 400kHz: ~4x faster touch polling than 100kHz
  
  // Initialize PCF8574 IO Expander outputs
  writePCF8574(pcf8574_state);
  
  // LCD screen hardware reset
  resetLCD();
  
  // Initialize LovyanGFX
  lcd.init();
  lcd.setRotation(0); // Portrait
  lcd.setTextDatum(MC_DATUM);
  
  // Backlight maximum on GPIO 6
  pinMode(LCD_BL_PIN, OUTPUT);
  analogWrite(LCD_BL_PIN, 255);
  
  // Allocate the off-screen car canvas in PSRAM (flicker-free, clean clears).
  carCanvas.setColorDepth(16);
  carCanvas.setPsram(true);
  useCanvasSprite = (carCanvas.createSprite(CANVAS_W, CANVAS_H) != nullptr);
  if (useCanvasSprite) {
    carCanvas.setTextDatum(MC_DATUM);
  } else {
    Serial.println("Car canvas sprite alloc failed - drawing directly (enable PSRAM in board options).");
  }

  // Boot / splash screen
  lcd.fillScreen(COLOR_BG);
  lcd.setTextDatum(MC_DATUM);
  lcd.setTextColor(COLOR_TEXT);
  lcd.setTextSize(3);
  lcd.drawString("AKKU", 160, 196);
  lcd.setTextColor(COLOR_CYAN);
  lcd.drawString("TESLA", 160, 232);
  lcd.setTextSize(1);
  lcd.setTextColor(COLOR_MUTED);
  lcd.drawString("CONNECTING TO FLAT403", 160, 286);

  // Begin Wi-Fi with an animated sweeping progress bar
  WiFi.begin(ssid, password);
  const int barX = 70, barY = 312, barW = 180;
  lcd.drawRoundRect(barX, barY, barW, 8, 4, COLOR_BORDER);
  int progress = 0;
  while (WiFi.status() != WL_CONNECTED) {
    lcd.fillRoundRect(barX + 2, barY + 2, barW - 4, 4, 2, COLOR_BG);
    lcd.fillRoundRect(barX + 2, barY + 2, progress, 4, 2, COLOR_CYAN);
    progress += 16;
    if (progress > barW - 4) progress = 8;
    delay(160);
  }

  lcd.fillRoundRect(barX + 2, barY + 2, barW - 4, 4, 2, COLOR_GREEN);
  lcd.setTextColor(COLOR_GREEN);
  lcd.drawString("CONNECTED", 160, 336);
  delay(700);

  // Start mDNS
  MDNS.begin("nextion-panel");
  resolveCarIP();

  // Spin up the network task on core 0 so HTTP/mDNS never block the UI loop
  // (which runs on core 1). Touch taps enqueue commands instead of sending
  // them inline.
  commandQueue = xQueueCreate(8, sizeof(CarCommand));
  xTaskCreatePinnedToCore(
    networkTask,    // task function
    "networkTask",  // name
    8192,           // stack (bytes) — HTTPClient + mDNS need headroom
    nullptr,        // params
    1,              // priority
    nullptr,        // task handle
    0               // core 0 (Arduino loop runs on core 1)
  );
  
  // Render main screen dashboard
  lcd.fillScreen(COLOR_BG);
  drawStatusBar();
  drawHeader();
  renderCarCanvas();
  drawButtons();
  
  lastTimeUpdate = millis();
  lastFlashTime = millis();
}

void loop() {
  // Update mock digital clock every 60s
  if (millis() - lastTimeUpdate >= 60000) {
    mockMinute++;
    if (mockMinute >= 60) {
      mockMinute = 0;
      mockHour++;
      if (mockHour >= 24) mockHour = 0;
    }
    drawStatusBar();
    lastTimeUpdate = millis();
  }

  // (mDNS retries are handled by networkTask on core 0 — see networkTask())

  // Animate blinker flashing (500ms cycle). Re-rendering the whole canvas is
  // cheap and flicker-free thanks to the off-screen sprite buffer.
  if ((blinker1On || blinker2On) && (millis() - lastFlashTime >= 500)) {
    flashState = !flashState;
    lastFlashTime = millis();
    renderCarCanvas();
  }

  // Detect WiFi / mDNS status changes and update header + status icons
  static bool lastCarResolved = false;
  static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
  if (WiFi.status() != lastWifiStatus || carResolved != lastCarResolved) {
    lastWifiStatus = WiFi.status();
    lastCarResolved = carResolved;
    drawStatusBar();
    drawHeader();
  }

  // Touch Screen parsing
  uint16_t tx = 0;
  uint16_t ty = 0;
  if (readTouch(tx, ty)) {
    uint16_t rawX = tx;
    uint16_t rawY = ty;
    
    // Apply calibration adjustments
    if (TOUCH_MIRROR_X) tx = 319 - tx;
    if (TOUCH_MIRROR_Y) ty = 479 - ty;
    if (TOUCH_SWAP_XY) {
      int temp = tx;
      tx = ty;
      ty = temp;
    }
    
    Serial.printf("Touch Detected! Raw: X=%d, Y=%d | Calibrated (Screen): X=%d, Y=%d\n", rawX, rawY, tx, ty);
    
    if (!lastPressed) {
      // --- CONTROL BUTTON GRID (matches BTN_* layout) ---
      // Row 1
      if (ty >= BTN_ROW1 && ty <= BTN_ROW1 + BTN_H) {
        if (tx >= BTN_X1 && tx <= BTN_X1 + BTN_W)      toggleBlinker1();
        else if (tx >= BTN_X2 && tx <= BTN_X2 + BTN_W) toggleBlinker2();
      }
      // Row 2
      else if (ty >= BTN_ROW2 && ty <= BTN_ROW2 + BTN_H) {
        if (tx >= BTN_X1 && tx <= BTN_X1 + BTN_W)      toggleHeadlight();
        else if (tx >= BTN_X2 && tx <= BTN_X2 + BTN_W) toggleTaillight();
      }

      // --- DIRECT CAR HOT-ZONES (tap the car illustration itself) ---
      // Canvas spans y=92..352, car centered near y=222.
      else if (tx >= 90 && tx <= 230 && ty >= 100 && ty <= 345) {
        if (ty <= 155)        toggleHeadlight();  // front
        else if (ty >= 295)   toggleTaillight();  // rear
        else if (tx <= 140)   toggleBlinker1();   // left flank
        else if (tx >= 180)   toggleBlinker2();   // right flank
      }
    }
    lastPressed = true;
  } else {
    lastPressed = false;
  }

  // Short yield: keeps touch polling snappy (~5ms) while letting the idle/
  // watchdog tasks run. The heavy network work is off on core 0 now.
  delay(5);
}

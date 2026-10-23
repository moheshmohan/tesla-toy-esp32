# Akku Tesla — Toy Car Wireless Light Control

A Tesla toy car whose lights (headlights, taillights, both indicators) can be
controlled wirelessly over WiFi, either from any browser or from a dedicated
touch-screen remote.

```
┌─────────────────────────┐        WiFi / mDNS / HTTP        ┌──────────────────────────┐
│  Display remote         │ ───────────────────────────────► │  Car (ESP32, Arduino)    │
│  ONX3248G035 (ESP32-S3) │   akku-tesla.local               │  WebServer on port 80    │
│  ESP-IDF + LVGL UI      │                                  │  GPIO ──► lights         │
└─────────────────────────┘                                  └──────────────────────────┘
        │                                                              ▲
        │              any phone/laptop browser (http://akku-tesla.local)
        └──────────────────────────────────────────────────────────────┘
```

## Repository layout

| Path | What it is |
|------|------------|
| [`car-esp32/wifi_control_new/`](car-esp32/wifi_control_new/) | **Current** car firmware — Arduino sketch serving a car-shaped touch UI in the browser |
| [`car-esp32/wifi_car_control/`](car-esp32/wifi_car_control/) | First version of the car firmware — plain HTML button page |
| [`display-remote/idf_car_control/`](display-remote/idf_car_control/) | **Current** remote — ESP-IDF + LVGL app for the ONX3248G035 touch display ([its README](display-remote/idf_car_control/README.md)) |
| [`display-remote/sketch_test/`](display-remote/sketch_test/) | Earlier Arduino/LovyanGFX prototype of the display remote |

## Car firmware (Arduino)

Flashed to the ESP32 inside the car. It joins your WiFi, advertises itself via
mDNS as `akku-tesla.local`, and serves a web page with a top-down car and toggle
buttons for each light. Indicators blink at ~1 Hz while on.

| Light | GPIO | HTTP endpoints |
|-------|------|----------------|
| Left indicator  | D0 | `/blinker1/on`, `/blinker1/off` |
| Right indicator | D1 | `/blinker2/on`, `/blinker2/off` |
| Taillight       | D3 | `/taillight/on`, `/taillight/off` |
| Headlight       | D4 | `/headlight/on`, `/headlight/off` |

Build with the Arduino IDE (ESP32 board package), after setting your WiFi
credentials at the top of the sketch.

## Display remote (ESP-IDF + LVGL)

An [OpenNextion ONX3248G035](https://github.com/OpenNextion/OpenNextion-SKU-ONX3248G035)
board (ESP32-S3, 3.5" 320×480 ST7796 display, CST826 touch) becomes a handheld
remote with an LVGL dashboard. Networking runs on its own task/core so the UI
never stalls. Build/flash instructions are in
[display-remote/idf_car_control/README.md](display-remote/idf_car_control/README.md).

## Setup

1. Set `YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD` in the car sketch
   ([wifi_control_new.ino](car-esp32/wifi_control_new/wifi_control_new.ino))
   and in the remote ([main/car_net.c](display-remote/idf_car_control/main/car_net.c)).
   Both devices must be on the same network.
2. Flash the car sketch to the car's ESP32.
3. Flash the remote firmware to the display board.
4. Control the lights from the remote, or open `http://akku-tesla.local` in a browser.

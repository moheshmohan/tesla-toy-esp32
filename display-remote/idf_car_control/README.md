# AKKU TESLA — ESP-IDF + LVGL car-control panel

A rewrite of the Arduino `sketch_test` car controller for the **ONX3248G035**
board (ESP32-S3 + ST7796 320×480 + CST826 touch + 8 MB PSRAM), using **ESP-IDF +
LVGL** for a smooth, animated, double-buffered UI like the vendor demo apps.

It controls the same `wifi_car_control` device over HTTP (mDNS host
`akku-tesla.local`, endpoints `/blinker1|2/on|off`, `/headlight/on|off`,
`/taillight/on|off`).

## Why this is smoother than the Arduino version
- **LVGL** with hardware double-buffered DMA rendering out of PSRAM (anti-aliased
  shapes, gradients, real box-shadow glow, smooth button states).
- **No UI stalls**: Wi-Fi/mDNS/HTTP run on a dedicated task on core 0 (`car_net`);
  button taps just enqueue a command and return instantly.

## Project layout
```
idf_car_control/
├── CMakeLists.txt          top-level project
├── sdkconfig.defaults      esp32s3, octal PSRAM, 16MB flash, 1kHz tick, LVGL fonts
├── partitions.csv          4 MB factory app
├── main/
│   ├── main.c              board bringup + start UI/net
│   ├── car_ui.c/.h         LVGL dashboard (status bar, car, 4 toggle buttons)
│   ├── car_net.c/.h        Wi-Fi STA + mDNS + async HTTP command queue
│   └── idf_component.yml   lvgl, esp_lvgl_port, esp_lcd_touch, mdns
└── components/board/       vendor hardware component (ST7796/CST826/PCF8574/LVGL port)
```

## Configuration
Wi-Fi and car host are at the top of `main/car_net.c`:
```c
#define WIFI_SSID    "YOUR_WIFI_SSID"
#define WIFI_PASS    "YOUR_WIFI_PASSWORD"
#define CAR_HOSTNAME "akku-tesla"
```

## Build / flash / monitor
```bash
. ~/esp/esp-idf/export.sh          # once per shell
cd ~/Documents/Arduino/nextion/idf_car_control
idf.py set-target esp32s3          # first time only
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor   # use your board's serial port
```
Find the port with `ls /dev/cu.*` (usually `/dev/cu.usbmodem*` or
`/dev/cu.wchusbserial*`). Exit the monitor with `Ctrl-]`.

/**
 * AKKU TESLA — car-control panel for the ONX3248G035 (ESP32-S3 + ST7796 + CST826).
 * ESP-IDF + LVGL port. Reuses the vendor `board` component for hardware bringup,
 * then runs a smooth LVGL UI (car_ui) and async networking (car_net).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "board.h"
#include "board_lcd.h"
#include "board_exio.h"
#include "board_touch.h"
#include "board_lvgl.h"

#include "car_ui.h"
#include "car_net.h"

static const char *TAG = "main";

void app_main(void) {
    // NVS (required by Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- Hardware bringup (vendor board component) ----
    boardIIcAddDevToNum0();   // I2C0 (touch + IO expander)
    expandIoInit();           // PCF8574 (drives LCD reset etc.)
    vTaskDelay(pdMS_TO_TICKS(50));

    spiLcdInit();                                       // ST7796 over SPI
    vlcdTouchInit(EXAMPLE_LCD_V_RES, EXAMPLE_LCD_H_RES); // CST826 touch
    lv_port_init();                                     // LVGL + double PSRAM buffers
    spiLcdBlinit();                                     // backlight PWM
    spiLcdSetBl(100);                                   // 100% brightness

    ESP_LOGI(TAG, "Display + LVGL ready");

    // ---- App ----
    car_net_init();   // Wi-Fi STA + mDNS + HTTP command task (core 0)
    car_ui_init();    // LVGL dashboard

    ESP_LOGI(TAG, "AKKU TESLA panel running");
}

#include "car_net.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "mdns.h"

// ===== Configuration (matches the Arduino sketch) =====
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASS      "YOUR_WIFI_PASSWORD"
#define CAR_HOSTNAME   "akku-tesla"   // mDNS name -> akku-tesla.local
#define PANEL_HOSTNAME "nextion-panel"

#define WIFI_CONNECTED_BIT BIT0
#define MDNS_RETRY_MS      10000
#define HTTP_TIMEOUT_MS    1500

static const char *TAG = "car_net";

typedef struct {
    char endpoint[40];
} car_cmd_t;

static EventGroupHandle_t s_wifi_events;
static QueueHandle_t      s_cmd_queue;
static volatile bool      s_wifi_connected = false;
static volatile bool      s_car_linked = false;
static char               s_car_ip[16] = {0};   // resolved car IP string

bool car_net_wifi_connected(void) { return s_wifi_connected; }
bool car_net_car_linked(void)     { return s_car_linked; }

// ---- Wi-Fi event handling -------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_car_linked = false;
        s_car_ip[0] = '\0';
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();   // keep retrying
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void) {
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---- mDNS resolve of the car ----------------------------------------------
static void resolve_car(void) {
    esp_ip4_addr_t addr = {0};
    esp_err_t err = mdns_query_a(CAR_HOSTNAME, 800, &addr);
    if (err == ESP_OK && addr.addr != 0) {
        snprintf(s_car_ip, sizeof(s_car_ip), IPSTR, IP2STR(&addr));
        s_car_linked = true;
        ESP_LOGI(TAG, "Resolved %s.local -> %s", CAR_HOSTNAME, s_car_ip);
    } else {
        s_car_linked = false;
        s_car_ip[0] = '\0';
        ESP_LOGW(TAG, "mDNS query for %s.local failed", CAR_HOSTNAME);
    }
}

// ---- HTTP command send -----------------------------------------------------
static void send_command(const char *endpoint) {
    if (!s_wifi_connected) return;

    // Use resolved IP if available, otherwise fall back to the .local name.
    char url[96];
    const char *host = (s_car_ip[0] != '\0') ? s_car_ip : (CAR_HOSTNAME ".local");
    snprintf(url, sizeof(url), "http://%s%s", host, endpoint);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_config_t cfg2 = cfg;  // keep a copy for the retry
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s -> %d", url, esp_http_client_get_status_code(client));
        esp_http_client_cleanup(client);
        return;
    }
    esp_http_client_cleanup(client);

    // One retry: the car runs a blocking single-client web server and can be
    // momentarily busy. A single fresh-connection retry recovers most misses.
    ESP_LOGW(TAG, "GET %s failed (%s), retrying once", url, esp_err_to_name(err));
    client = esp_http_client_init(&cfg2);
    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s -> %d (retry)", url, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "GET %s failed again: %s", url, esp_err_to_name(err));
        s_car_linked = false;   // force a fresh mDNS resolve next round
        s_car_ip[0] = '\0';
    }
    esp_http_client_cleanup(client);
}

// ---- Network task ----------------------------------------------------------
static void net_task(void *arg) {
    // Wait until Wi-Fi is up before the first resolve.
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(PANEL_HOSTNAME);
    resolve_car();

    TickType_t last_retry = xTaskGetTickCount();
    for (;;) {
        // Periodically (re)resolve the car if not linked.
        if (!s_car_linked && s_wifi_connected &&
            (xTaskGetTickCount() - last_retry) > pdMS_TO_TICKS(MDNS_RETRY_MS)) {
            resolve_car();
            last_retry = xTaskGetTickCount();
        }

        car_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
            send_command(cmd.endpoint);
        }
    }
}

// ---- Public API ------------------------------------------------------------
void car_net_send(const char *endpoint) {
    if (!s_cmd_queue || !endpoint) return;
    car_cmd_t cmd;
    strncpy(cmd.endpoint, endpoint, sizeof(cmd.endpoint) - 1);
    cmd.endpoint[sizeof(cmd.endpoint) - 1] = '\0';
    xQueueSend(s_cmd_queue, &cmd, 0);   // drop if full, never block the UI
}

void car_net_init(void) {
    s_cmd_queue = xQueueCreate(8, sizeof(car_cmd_t));
    wifi_start();
    xTaskCreatePinnedToCore(net_task, "car_net", 6144, NULL, 5, NULL, 0);
}

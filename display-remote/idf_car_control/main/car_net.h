#pragma once
#include <stdbool.h>

/* Wi-Fi + car connection. Networking runs on its own task; the UI never blocks
 * on it. Call car_net_init() once at startup, then car_net_send() from UI
 * callbacks to fire-and-forget a command to the car. */

#ifdef __cplusplus
extern "C" {
#endif

void car_net_init(void);

/* Enqueue an HTTP GET to the car, e.g. "/headlight/on". Non-blocking. */
void car_net_send(const char *endpoint);

/* Status getters for the UI to poll (cheap, lock-free). */
bool car_net_wifi_connected(void);
bool car_net_car_linked(void);

#ifdef __cplusplus
}
#endif

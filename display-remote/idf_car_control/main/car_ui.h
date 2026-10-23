#pragma once

/* Builds the LVGL car-control dashboard. Must be called after lv_port_init()
 * (i.e. after the LVGL port task exists). Thread-safety is handled inside. */

#ifdef __cplusplus
extern "C" {
#endif

void car_ui_init(void);

#ifdef __cplusplus
}
#endif

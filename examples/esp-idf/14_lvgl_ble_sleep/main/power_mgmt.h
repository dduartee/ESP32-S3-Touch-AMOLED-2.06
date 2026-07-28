#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pm_configure_auto_light_sleep(void);

esp_err_t pm_set_light_sleep(bool enable);

esp_err_t display_sleep_init(lv_display_t *disp);

void display_sleep_enter(void);

void display_sleep_exit(void);

esp_lcd_panel_handle_t display_get_panel(void);

#ifdef __cplusplus
}
#endif

#endif /* POWER_MGMT_H */

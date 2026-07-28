#include "power_mgmt.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pm";

static esp_lcd_panel_handle_t    s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle    = NULL;
static bool s_display_sleep_ready = false;

struct lvgl_port_disp_ctx {
    int                        disp_type;
    esp_lcd_panel_io_handle_t  io_handle;
    esp_lcd_panel_handle_t     panel_handle;
    esp_lcd_panel_handle_t     control_handle;
};

esp_err_t display_sleep_init(lv_display_t *disp)
{
    if (disp == NULL) {
        ESP_LOGE(TAG, "display_sleep_init: disp is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    struct lvgl_port_disp_ctx *ctx =
        (struct lvgl_port_disp_ctx *)lv_display_get_driver_data(disp);

    if (ctx == NULL) {
        ESP_LOGE(TAG, "display_sleep_init: driver_data is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    s_panel_handle = ctx->panel_handle;
    s_io_handle    = ctx->io_handle;

    if (s_panel_handle == NULL || s_io_handle == NULL) {
        ESP_LOGE(TAG, "display_sleep_init: panel_handle=%p io_handle=%p",
                 (void *)s_panel_handle, (void *)s_io_handle);
        return ESP_ERR_INVALID_STATE;
    }

    s_display_sleep_ready = true;
    ESP_LOGI(TAG, "display_sleep_init: panel=%p io=%p",
             (void *)s_panel_handle, (void *)s_io_handle);
    return ESP_OK;
}

void display_sleep_enter(void)
{
    if (!s_display_sleep_ready) {
        ESP_LOGW(TAG, "display_sleep_enter: not initialized");
        return;
    }

    ESP_LOGI(TAG, "display_sleep_enter");

    bsp_display_backlight_off();
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_panel_disp_on_off(s_panel_handle, false);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_panel_io_tx_param(s_io_handle, 0x10, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

void display_sleep_exit(void)
{
    if (!s_display_sleep_ready) {
        ESP_LOGW(TAG, "display_sleep_exit: not initialized");
        return;
    }

    ESP_LOGI(TAG, "display_sleep_exit");

    esp_lcd_panel_io_tx_param(s_io_handle, 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_panel_disp_on_off(s_panel_handle, true);
    vTaskDelay(pdMS_TO_TICKS(10));

    bsp_display_backlight_on();
}

esp_err_t pm_configure_auto_light_sleep(void)
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };

    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Power Management: Auto Light Sleep ENABLED");
    ESP_LOGI(TAG, "  Max CPU freq: %d MHz", pm_config.max_freq_mhz);
    ESP_LOGI(TAG, "  Min CPU freq: %d MHz", pm_config.min_freq_mhz);

    return ESP_OK;
}

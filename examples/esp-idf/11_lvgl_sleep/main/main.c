#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "lvgl_sleep";

#define PIN_I2C_SCL     GPIO_NUM_14
#define PIN_I2C_SDA     GPIO_NUM_15
#define PIN_LCD_CS      GPIO_NUM_12
#define PIN_LCD_PCLK    GPIO_NUM_11
#define PIN_LCD_D0      GPIO_NUM_4
#define PIN_LCD_D1      GPIO_NUM_5
#define PIN_LCD_D2      GPIO_NUM_6
#define PIN_LCD_D3      GPIO_NUM_7
#define PIN_LCD_RST     GPIO_NUM_8
#define PIN_TP_RST      GPIO_NUM_9
#define PIN_TP_INT      GPIO_NUM_38
#define PIN_BOOT        GPIO_NUM_0

#define LCD_H_RES       410
#define LCD_V_RES       502
#define SLEEP_TIMEOUT_S 10

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_touch_handle_t tp = NULL;
static lv_obj_t *label_info = NULL;
static lv_obj_t *label_counter = NULL;

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static void rounder_cb(lv_event_t *e) {
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void set_backlight(uint8_t val) {
    uint32_t cmd = (0x51 & 0xFF) << 8 | (0x02 << 24);
    esp_lcd_panel_io_tx_param(io_handle, cmd, &val, 1);
}

static void display_sleep(void) {
    ESP_LOGI(TAG, "display_sleep");
    set_backlight(0);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_lcd_panel_disp_on_off(panel_handle, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_lcd_panel_io_tx_param(io_handle, 0x10, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void display_wake(void) {
    ESP_LOGI(TAG, "display_wake");
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_disp_on_off(panel_handle, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    set_backlight(255);
}

static void create_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);

    label_info = lv_label_create(scr);
    lv_label_set_text(label_info,
        "LVGL + Light Sleep\n"
        "BOOT button to wake");
    lv_obj_center(label_info);

    label_counter = lv_label_create(scr);
    lv_label_set_text(label_counter, "");
    lv_obj_align(label_counter, LV_ALIGN_BOTTOM_MID, 0, -30);
}

static void update_counter(uint32_t sec) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Awake: %lu s", (unsigned long)sec);
    lv_label_set_text(label_counter, buf);
}

static void init_display(void) {
    spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0x16, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

static void init_touch(void) {
    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = -1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    i2c_master_bus_handle_t i2c_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(
        i2c_handle, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
}

static void init_lvgl(void) {
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_max_sleep_ms = 500;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * 100,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {
            .sw_rotate = true,
            .buff_dma = false,
            .swap_bytes = true,
        },
    };
    lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {.bb_mode = 0, .avoid_tearing = false},
    };
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };
    lvgl_port_add_touch(&touch_cfg);
}

void app_main(void) {
    ESP_LOGI(TAG, "LVGL + Light Sleep Example");
    ESP_LOGI(TAG, "SH8601 410x502, FT5x06 touch, wake on BOOT (GPIO0)");

    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_BOOT, GPIO_PULLUP_ONLY));

    init_display();
    init_touch();
    init_lvgl();

    lvgl_port_lock(0);
    create_ui();
    update_counter(0);
    lvgl_port_unlock();

    uint32_t awake_secs = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        awake_secs++;

        lvgl_port_lock(0);
        update_counter(awake_secs);
        lvgl_port_unlock();

        if (awake_secs >= SLEEP_TIMEOUT_S) {
            ESP_LOGI(TAG, "Idle timeout -- entering light sleep");

            lvgl_port_lock(0);
            lv_obj_set_style_bg_color(
                lv_screen_active(), lv_color_hex(0x000000), LV_PART_MAIN);
            lvgl_port_unlock();
            vTaskDelay(pdMS_TO_TICKS(150));

            display_sleep();
            lvgl_port_stop();

            esp_sleep_enable_ext0_wakeup(PIN_BOOT, 0);

            ESP_LOGI(TAG, "Light sleep -- press BOOT to wake");
            fflush(stdout);
            esp_light_sleep_start();

            ESP_LOGI(TAG, "Woke from light sleep");

            lvgl_port_resume();
            display_wake();

            awake_secs = 0;

            lvgl_port_lock(0);
            lv_obj_set_style_bg_color(
                lv_screen_active(), lv_color_hex(0x000000), LV_PART_MAIN);
            update_counter(0);
            lvgl_port_unlock();
        }
    }
}

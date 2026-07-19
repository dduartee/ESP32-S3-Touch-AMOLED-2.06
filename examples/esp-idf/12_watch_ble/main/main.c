#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_check.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "bt_nus.h"
#include "pcf85063.h"

static const char *TAG = "watch_ble";

/* Pin definitions */
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

/* AXP2101 PMU I2C address */
#define AXP2101_ADDR    0x34

/* WiFi */
#define WIFI_SSID       "gaabe"
#define WIFI_PASS       "tetetectec"
#define WIFI_TIMEOUT_MS 15000

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* NVS keys */
#define NVS_NAMESPACE   "storage"
#define NVS_KEY_BOOT    "boot_count"
#define NVS_KEY_NTP_TS  "ntp_sync_ts"
#define NTP_RESYNC_DAYS 7

typedef enum {
    NTP_NONE,
    NTP_NEEDED,
    NTP_CONNECTING,
    NTP_SYNCING,
    NTP_DONE
} ntp_state_t;

static ntp_state_t ntp_state = NTP_NONE;
static bool wifi_inited = false;
static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t pmu_dev = NULL;

static pcf85063_config_t rtc_cfg = {
    .i2c_bus = NULL,
    .address = 0x51,
};

/* LVGL objects */
static lv_obj_t *lbl_time;
static lv_obj_t *lbl_date;
static lv_obj_t *lbl_status;

static const char *weekday_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/* Display handles */
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_touch_handle_t tp = NULL;

/* ── SH8601 init commands ── */
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

/* ── Rounder callback (SH8601 needs even coords) ── */
static void rounder_cb(lv_event_t *e) {
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

/* ── Backlight ── */
static void set_backlight(uint8_t val) {
    uint32_t cmd = (0x51 & 0xFF) << 8 | (0x02 << 24);
    esp_lcd_panel_io_tx_param(io_handle, cmd, &val, 1);
}

/* ── Display sleep/wake ── */
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

/* ── I2C master init (shared bus: PMU + RTC + touch) ── */
static esp_err_t i2c_init(void) {
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    return i2c_new_master_bus(&cfg, &i2c_bus);
}

/* ── AXP2101 PMU register read/write ── */
static esp_err_t pmu_read_reg(uint8_t reg, uint8_t *val) {
    return i2c_master_transmit_receive(pmu_dev, &reg, 1, val, 1, 1000);
}

static esp_err_t pmu_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(pmu_dev, buf, 2, 1000);
}

static uint16_t pmu_read_16(uint8_t reg) {
    uint8_t lo, hi;
    pmu_read_reg(reg, &hi);
    pmu_read_reg(reg + 1, &lo);
    return ((uint16_t)hi << 4) | lo;
}

static esp_err_t pmu_init(void) {
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev, &pmu_dev),
                        TAG, "PMU device add failed");

    uint8_t id = 0;
    if (pmu_read_reg(0x03, &id) != ESP_OK) {
        ESP_LOGE(TAG, "PMU not responding");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AXP2101 PMU found, chip ID: 0x%02X", id);

    pmu_write_reg(0x92, 0x00); /* disable TS pin */
    pmu_write_reg(0x82, 0xFF); /* enable all ADC channels */
    pmu_write_reg(0x84, 0x02); /* enable VBUS ADC */
    pmu_write_reg(0x21, 0x20); /* disable all IRQs */
    pmu_write_reg(0x22, 0x20);
    pmu_write_reg(0x23, 0x20);
    pmu_write_reg(0x24, 0x20);
    pmu_write_reg(0x25, 0x20);
    pmu_write_reg(0x27, 0x20);
    pmu_read_reg(0x01, &id); /* clear IRQ by reading */
    pmu_read_reg(0x02, &id);

    return ESP_OK;
}

static void pmu_log_status(void) {
    uint16_t batt_mv = pmu_read_16(0x34);
    uint8_t  pct = 0;
    pmu_read_reg(0xB8, &pct);
    uint8_t  status = 0;
    pmu_read_reg(0x00, &status);
    bool vbus_in = (status & 0x20) != 0;
    bool charging = (status & 0x04) != 0;
    uint16_t vbus_mv = pmu_read_16(0x32);
    uint16_t sys_mv = pmu_read_16(0x30);

    ESP_LOGI(TAG, "Battery: %umV %u%% VBUS: %s %umV System: %umV %s",
             batt_mv, pct, vbus_in ? "yes" : "no", vbus_mv, sys_mv,
             charging ? "CHARGING" : "");
}

/* ── WiFi / NTP ── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (ntp_state == NTP_CONNECTING) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ntp_state = NTP_SYNCING;
    }
}

static void wifi_start(void) {
    if (wifi_inited) return;
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    wifi_config_t w = { .sta = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASS,
    }};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &w));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_inited = true;
    ntp_state = NTP_CONNECTING;
}

static void wifi_stop(void) {
    if (!wifi_inited) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    wifi_inited = false;
    if (wifi_event_group) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }
}

static esp_err_t sync_rtc_from_ntp(void) {
    ESP_LOGI(TAG, "SNTP sync...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    time_t now = 0;
    struct tm ti = {0};
    int retry = 0;
    while (retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &ti);
        if (ti.tm_year >= (2026 - 1900)) break;
        retry++;
    }
    esp_sntp_stop();
    if (ti.tm_year < (2026 - 1900)) {
        ESP_LOGE(TAG, "NTP failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "NTP: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    pcf85063_time_t rt = {
        .second = ti.tm_sec, .minute = ti.tm_min, .hour = ti.tm_hour,
        .day = ti.tm_mday, .weekday = ti.tm_wday,
        .month = ti.tm_mon + 1, .year = ti.tm_year + 1900,
    };
    pcf85063_set_time(&rtc_cfg, &rt);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        time(&now);
        nvs_set_i64(nvs, NVS_KEY_NTP_TS, now);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return ESP_OK;
}

/* ── Display init ── */
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
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_cfg.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_cfg, &tp_io));
    esp_lcd_touch_config_t cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io, &cfg, &tp));
}

static void init_lvgl(void) {
    lvgl_port_cfg_t lcfg = ESP_LVGL_PORT_INIT_CONFIG();
    lcfg.task_max_sleep_ms = 500;
    ESP_ERROR_CHECK(lvgl_port_init(&lcfg));

    lvgl_port_display_cfg_t dcfg = {
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
    lv_display_t *disp = lvgl_port_add_disp(&dcfg);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lvgl_port_touch_cfg_t tcfg = {
        .disp = disp,
        .handle = tp,
    };
    lvgl_port_add_touch(&tcfg);
}

/* ── UI ── */
static void create_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);

    lbl_time = lv_label_create(scr);
    lv_label_set_text(lbl_time, "00:00:00");
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, -60);

    lbl_date = lv_label_create(scr);
    lv_label_set_text(lbl_date, "---");
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, 0);

    lbl_status = lv_label_create(scr);
    lv_label_set_text(lbl_status, "");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 50);
}

static void update_rtc_display(void) {
    pcf85063_time_t t;
    if (pcf85063_get_time(&rtc_cfg, &t) != ESP_OK) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
    lv_label_set_text(lbl_time, buf);
    snprintf(buf, sizeof(buf), "%s %d/%d/%d",
             weekday_names[t.weekday], t.day, t.month, t.year);
    lv_label_set_text(lbl_date, buf);
}

static void update_status_display(void) {
    uint8_t pct = 0;
    pmu_read_reg(0xB8, &pct);
    bool ble_conn = bt_nus_is_connected();
    char buf[48];
    snprintf(buf, sizeof(buf), "BLE: %s  BAT: %u%%",
             ble_conn ? "CONN" : "ADV", pct);
    lv_label_set_text(lbl_status, buf);
}

/* ── Main ── */
void app_main(void) {
    ESP_LOGI(TAG, "Watch BLE + Light Sleep");

    setenv("TZ", "BRT3", 1);
    tzset();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(i2c_init());
    ESP_ERROR_CHECK(pmu_init());

    init_display();
    init_touch();
    init_lvgl();
    pmu_log_status();

    /* RTC */
    rtc_cfg.i2c_bus = i2c_bus;
    if (pcf85063_init(&rtc_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "RTC init failed");
    } else {
        ESP_LOGI(TAG, "RTC initialized");
    }

    /* Create UI */
    lvgl_port_lock(0);
    create_ui();
    update_rtc_display();
    update_status_display();
    lvgl_port_unlock();

    /* Check NTP need */
    pcf85063_time_t rtc_now;
    bool rtc_valid = (pcf85063_get_time(&rtc_cfg, &rtc_now) == ESP_OK);
    bool need_sync = false;

    if (!rtc_valid || rtc_now.year < 2026) {
        need_sync = true;
    } else {
        struct tm tm = {
            .tm_year = rtc_now.year - 1900,
            .tm_mon = rtc_now.month - 1,
            .tm_mday = rtc_now.day,
            .tm_hour = rtc_now.hour,
            .tm_min = rtc_now.minute,
            .tm_sec = rtc_now.second,
        };
        time_t rtc_epoch = mktime(&tm);
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            int64_t last = 0;
            nvs_get_i64(nvs, NVS_KEY_NTP_TS, &last);
            nvs_close(nvs);
            if (last == 0 || (rtc_epoch - last) >= (NTP_RESYNC_DAYS * 86400))
                need_sync = true;
        } else {
            need_sync = true;
        }
    }

    if (need_sync) {
        ntp_state = NTP_NEEDED;
        wifi_start();
    }

    /* Start BLE */
    bt_nus_init();
    ESP_LOGI(TAG, "BLE NUS advertising");

    /* Main loop */
    uint32_t idle_secs = 0;
    bool sleeping = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (sleeping) continue;

        idle_secs++;

        lvgl_port_lock(0);
        if (idle_secs % 1 == 0) update_rtc_display();
        if (idle_secs % 5 == 0) update_status_display();
        lvgl_port_unlock();

        /* NTP sync when WiFi connects */
        if (ntp_state == NTP_SYNCING) {
            ESP_LOGI(TAG, "NTP sync...");
            sync_rtc_from_ntp();
            wifi_stop();
            ntp_state = NTP_DONE;
            lvgl_port_lock(0);
            update_rtc_display();
            lvgl_port_unlock();
        }

        /* WiFi connect timeout (30s) */
        if (idle_secs == 30 && ntp_state == NTP_CONNECTING) {
            wifi_stop();
            ntp_state = NTP_NONE;
        }

        /* Idle → light sleep */
        if (idle_secs >= SLEEP_TIMEOUT_S) {
            if (wifi_inited) wifi_stop();

            ESP_LOGI(TAG, "Light sleep...");

            lvgl_port_stop();
            vTaskDelay(pdMS_TO_TICKS(200));

            display_sleep();
            esp_sleep_pd_config(ESP_PD_DOMAIN_MODEM, ESP_PD_OPTION_ON);
            esp_sleep_enable_ext0_wakeup(PIN_BOOT, 0);

            sleeping = true;
            ESP_LOGI(TAG, "Press BOOT to wake");
            fflush(stdout);
            esp_light_sleep_start();

            ESP_LOGI(TAG, "Woke");
            lvgl_port_resume();
            display_wake();
            sleeping = false;
            idle_secs = 0;

            lvgl_port_lock(0);
            lv_obj_set_style_bg_color(
                lv_screen_active(), lv_color_hex(0x000000), LV_PART_MAIN);
            update_rtc_display();
            update_status_display();
            lvgl_port_unlock();
        }
    }
}

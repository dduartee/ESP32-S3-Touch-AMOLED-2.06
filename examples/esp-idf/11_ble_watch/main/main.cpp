#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include "pcf85063.h"
#include "bt_nus.h"

#define TAG "ble_watch"

#define DISPLAY_IDLE_TIMEOUT_MS 10000
#define HEARTBEAT_INTERVAL_MS   2000
#define BOOT_WAKEUP_GPIO       GPIO_NUM_0

#define I2C_MASTER_TIMEOUT_MS 1000

#define NVS_NAMESPACE "storage"
#define NVS_KEY_BOOT_COUNT "boot_count"

#define WIFI_SSID     "gaabe"
#define WIFI_PASS     "tetetectec"
#define WIFI_TIMEOUT_MS 15000

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

#define NVS_KEY_NTP_SYNC_TS "ntp_sync_ts"
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

static i2c_master_dev_handle_t pmu_dev_handle = NULL;

static pcf85063_config_t rtc_cfg = {
    .i2c_bus = NULL,
    .address = 0x51,
};

static lv_obj_t *lbl_time;
static lv_obj_t *lbl_date;
static lv_obj_t *lbl_status;
static lv_obj_t *lbl_notification = NULL;
static lv_obj_t *lbl_battery;

static bool display_on = true;
static uint32_t display_idle_ms = 0;

static const char *weekday_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *wakeup_cause_to_str(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:    return "UNDEFINED";
        case ESP_SLEEP_WAKEUP_ALL:          return "ALL";
        case ESP_SLEEP_WAKEUP_EXT0:         return "EXT0";
        case ESP_SLEEP_WAKEUP_EXT1:         return "EXT1";
        case ESP_SLEEP_WAKEUP_TIMER:        return "TIMER";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:     return "TOUCHPAD";
        case ESP_SLEEP_WAKEUP_ULP:          return "ULP";
        case ESP_SLEEP_WAKEUP_GPIO:         return "GPIO";
        case ESP_SLEEP_WAKEUP_UART:         return "UART";
        case ESP_SLEEP_WAKEUP_WIFI:         return "WIFI";
        case ESP_SLEEP_WAKEUP_COCPU:        return "COCPU";
        case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG: return "COCPU_TRIG";
        case ESP_SLEEP_WAKEUP_BT:           return "BT";
        default:                            return "UNKNOWN";
    }
}

static int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    esp_err_t ret = i2c_master_transmit_receive(pmu_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU read failed (reg 0x%02x)", regAddr);
        return -1;
    }
    return 0;
}

static int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(buffer + 1, data, len);

    esp_err_t ret = i2c_master_transmit(pmu_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU write failed (reg 0x%02x)", regAddr);
        return -1;
    }
    return 0;
}

static esp_err_t i2c_init_pmu(void)
{
    i2c_master_bus_handle_t bsp_bus = bsp_i2c_get_handle();
    if (!bsp_bus) {
        ESP_LOGE(TAG, "BSP I2C bus not available");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_SLAVE_ADDRESS,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0
        }
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bsp_bus, &dev_config, &pmu_dev_handle));
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (ntp_state == NTP_CONNECTING) {
            ESP_LOGW(TAG, "WiFi disconnected, retrying...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ntp_state = NTP_SYNCING;
    }
}

static void wifi_start(void)
{
    if (wifi_inited) return;

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_inited = true;
    ntp_state = NTP_CONNECTING;
    ESP_LOGI(TAG, "WiFi connecting (non-blocking)...");
}

static void wifi_stop(void)
{
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

static esp_err_t sync_rtc_from_ntp(void)
{
    ESP_LOGI(TAG, "Syncing time via SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = {};
    int retry = 0;
    while (retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2026 - 1900)) {
            break;
        }
        retry++;
    }
    esp_sntp_stop();

    if (timeinfo.tm_year < (2026 - 1900)) {
        ESP_LOGE(TAG, "Failed to get NTP time after %d retries", retry);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "NTP time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    pcf85063_time_t rtc_time = {};
    rtc_time.second = timeinfo.tm_sec;
    rtc_time.minute = timeinfo.tm_min;
    rtc_time.hour   = timeinfo.tm_hour;
    rtc_time.day    = timeinfo.tm_mday;
    rtc_time.weekday = timeinfo.tm_wday;
    rtc_time.month  = timeinfo.tm_mon + 1;
    rtc_time.year   = timeinfo.tm_year + 1900;

    ESP_ERROR_CHECK(pcf85063_set_time(&rtc_cfg, &rtc_time));

    pcf85063_time_t verify = {};
    pcf85063_get_time(&rtc_cfg, &verify);
    ESP_LOGI(TAG, "RTC set to: %04d-%02d-%02d %02d:%02d:%02d",
             verify.year, verify.month, verify.day,
             verify.hour, verify.minute, verify.second);

    /* Mark NTP synced in NVS with current Unix timestamp */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        time_t now;
        time(&now);
        ESP_LOGI(TAG, "Storing NTP sync timestamp: %lld", (long long)now);
        nvs_set_i64(nvs, NVS_KEY_NTP_SYNC_TS, now);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    return ESP_OK;
}

static void create_ui(void)
{
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
    lv_label_set_text(lbl_status, "Advertising");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 50);

    lbl_notification = lv_label_create(scr);
    lv_label_set_text(lbl_notification, "Waiting for notifications...");
    lv_obj_set_style_text_font(lbl_notification, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_notification, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_align(lbl_notification, LV_ALIGN_CENTER, 0, 80);

    lbl_battery = lv_label_create(scr);
    lv_label_set_text(lbl_battery, "Battery: --%");
    lv_obj_set_style_text_font(lbl_battery, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_battery, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(lbl_battery, LV_ALIGN_CENTER, 0, 110);
}

static void update_rtc_display(void)
{
    pcf85063_time_t t;
    if (pcf85063_get_time(&rtc_cfg, &t) != ESP_OK) {
        return;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
    lv_label_set_text(lbl_time, buf);

    snprintf(buf, sizeof(buf), "%s %d/%d/%d", weekday_names[t.weekday], t.day, t.month, t.year);
    lv_label_set_text(lbl_date, buf);
}

static void display_off_action(void) {
    display_on = false;
    bsp_display_lock(0);
    lv_obj_clean(lv_screen_active());
    bsp_display_unlock();
    bsp_display_backlight_off();
    ESP_LOGI(TAG, "Display OFF");
}

static void display_on_action(void) {
    bsp_display_backlight_on();
    bsp_display_lock(0);
    create_ui();
    update_rtc_display();
    bsp_display_unlock();
    display_on = true;
    display_idle_ms = 0;
    ESP_LOGI(TAG, "Display ON");
}

static void update_status_display(void) {
    if (!lbl_status) return;
    const char *ble_state = bt_nus_is_connected() ? "Connected" : "Advertising";
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", ble_state);
    lv_label_set_text(lbl_status, buf);
}

static void show_notification(const char *msg) {
    if (!lbl_notification) return;
    lv_label_set_text(lbl_notification, msg);
}

static void button_task(void *pv) {
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_WAKEUP_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&btn_cfg);

    int debounce = 0;
    bool prev_raw = true;
    bool pressed = false;

    while (1) {
        bool raw = gpio_get_level(BOOT_WAKEUP_GPIO);
        if (raw == prev_raw) {
            debounce = 0;
        } else {
            debounce++;
            if (debounce >= 3) {
                debounce = 0;
                prev_raw = raw;
                if (!raw && !pressed) {
                    pressed = true;
                    ESP_LOGI(TAG, "BOOT button pressed");
                    if (bt_nus_is_connected()) {
                        bt_nus_send((const uint8_t *)">cmd:btn_press\n", 15);
                    }
                    display_on_action();
                } else if (raw) {
                    pressed = false;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void ble_heartbeat_task(void *pv) {
    uint32_t count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
        count++;
        if (bt_nus_is_connected()) {
            char buf[64];
            int len = snprintf(buf, sizeof(buf), ">NUS alive count=%lu\n", (unsigned long)count);
            bt_nus_send((const uint8_t *)buf, len);
        }
    }
}

static void display_manager_task(void *pv) {
    while (1) {
        /* Check for NUS RX messages */
        const char *rx = bt_nus_last_rx();
        if (rx != NULL) {
            ESP_LOGI(TAG, "Notification: %s", rx);
            char display_buf[256];
            snprintf(display_buf, sizeof(display_buf), "%s", rx);
            bsp_display_lock(0);
            show_notification(display_buf);
            bsp_display_unlock();
            if (!display_on) {
                display_on_action();
            }
            display_idle_ms = 0;
        }

        /* Check if display should turn off */
        if (display_on && display_idle_ms >= DISPLAY_IDLE_TIMEOUT_MS) {
            display_off_action();
        }

        /* If display is off, enter light sleep */
        if (!display_on) {
            gpio_wakeup_enable(BOOT_WAKEUP_GPIO, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();
            esp_sleep_enable_timer_wakeup(60000000ULL); /* 60s periodic wake */
            esp_sleep_enable_bt_wakeup();

            ESP_LOGI(TAG, "Entering light sleep...");
            esp_light_sleep_start();

            esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
            ESP_LOGI(TAG, "Woke from light sleep: cause=%d", cause);

            /* Always turn on display on any wake */
            display_on_action();
        }

        vTaskDelay(pdMS_TO_TICKS(250));
        if (display_on) {
            display_idle_ms += 250;
        }
    }
}

static void rtc_update_task(void *pv) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (display_on) {
            bsp_display_lock(0);
            update_rtc_display();
            update_status_display();
            bsp_display_unlock();
        }
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  BLE Watch (Light Sleep)");
    ESP_LOGI(TAG, "========================================");

    /* Reset reason */
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char *reason_str;
    switch (reset_reason) {
        case ESP_RST_POWERON:    reason_str = "POWERON"; break;
        case ESP_RST_EXT:        reason_str = "EXT"; break;
        case ESP_RST_SW:         reason_str = "SW"; break;
        case ESP_RST_PANIC:      reason_str = "PANIC"; break;
        case ESP_RST_INT_WDT:    reason_str = "INT_WDT"; break;
        case ESP_RST_TASK_WDT:   reason_str = "TASK_WDT"; break;
        case ESP_RST_WDT:        reason_str = "WDT"; break;
        case ESP_RST_DEEPSLEEP:  reason_str = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT:   reason_str = "BROWNOUT"; break;
        default:                 reason_str = "UNKNOWN"; break;
    }
    ESP_LOGI(TAG, "Reset reason: %s", reason_str);

    /* Wakeup cause */
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wakeup cause: %s (0x%x)", wakeup_cause_to_str(wakeup_cause), wakeup_cause);

    /* Brazil timezone */
    setenv("TZ", "BRT3", 1);
    tzset();

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Boot counter */
    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        int32_t boot_count = 0;
        nvs_get_i32(nvs_handle, NVS_KEY_BOOT_COUNT, &boot_count);
        boot_count++;
        ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, NVS_KEY_BOOT_COUNT, boot_count));
        ESP_ERROR_CHECK(nvs_commit(nvs_handle));
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Boot count: %ld", (long)boot_count);
    }

    /* Display */
    bsp_display_start();
    ESP_LOGI(TAG, "Display initialized");

    /* PMU */
    ESP_ERROR_CHECK(i2c_init_pmu());
    ESP_LOGI(TAG, "PMU I2C device added");

    /* RTC */
    rtc_cfg.i2c_bus = bsp_i2c_get_handle();
    esp_err_t rtc_ret = pcf85063_init(&rtc_cfg);
    if (rtc_ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 init failed: %s", esp_err_to_name(rtc_ret));
    } else {
        ESP_LOGI(TAG, "PCF85063 RTC initialized");
    }

    /* LVGL UI */
    bsp_display_lock(0);
    create_ui();
    update_rtc_display();
    bsp_display_unlock();

    /* AXP2101 battery */
    XPowersPMU PMU;
    if (!PMU.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte)) {
        ESP_LOGE(TAG, "AXP2101 init failed");
    } else {
        ESP_LOGI(TAG, "AXP2101 initialized");
        PMU.disableTSPinMeasure();
        PMU.enableBattVoltageMeasure();
        PMU.enableVbusVoltageMeasure();
        PMU.enableSystemVoltageMeasure();
        PMU.enableTemperatureMeasure();
        uint16_t batt_mv = PMU.getBattVoltage();
        uint8_t batt_pct = PMU.getBatteryPercent();
        ESP_LOGI(TAG, "Battery: %u mV, %u%%", batt_mv, batt_pct);
        PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        PMU.clearIrqStatus();
    }

    /* BLE NUS */
    bt_nus_init();
    ESP_LOGI(TAG, "BLE NUS initialized");

    /* NTP sync check */
    pcf85063_time_t rtc_now;
    bool rtc_valid = (pcf85063_get_time(&rtc_cfg, &rtc_now) == ESP_OK);
    bool need_sync = false;
    if (!rtc_valid || rtc_now.year < 2026) {
        need_sync = true;
    } else {
        struct tm tm = {0};
        tm.tm_year = rtc_now.year - 1900;
        tm.tm_mon  = rtc_now.month - 1;
        tm.tm_mday = rtc_now.day;
        tm.tm_hour = rtc_now.hour;
        tm.tm_min  = rtc_now.minute;
        tm.tm_sec  = rtc_now.second;
        tm.tm_isdst = -1;
        time_t rtc_epoch = mktime(&tm);
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            int64_t last_sync = 0;
            nvs_get_i64(nvs, NVS_KEY_NTP_SYNC_TS, &last_sync);
            nvs_close(nvs);
            if (last_sync == 0 || (rtc_epoch - last_sync) >= (NTP_RESYNC_DAYS * 86400)) {
                need_sync = true;
            }
        } else {
            need_sync = true;
        }
    }
    if (need_sync) {
        ntp_state = NTP_NEEDED;
    }

    /* Start WiFi if NTP sync needed */
    if (ntp_state == NTP_NEEDED) {
        wifi_start();
    }

    /* Tasks */
    xTaskCreatePinnedToCore(button_task, "button", 2560, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(ble_heartbeat_task, "ble_heartbeat", 3072, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(rtc_update_task, "rtc_update", 3072, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(display_manager_task, "display_mgr", 4096, NULL, 1, NULL, 1);

    ESP_LOGI(TAG, "All tasks started, entering main loop");

    /* Main loop: handle NTP sync completion */
    int wifi_elapsed_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wifi_elapsed_ms += 1000;

        if (ntp_state == NTP_SYNCING) {
            ESP_LOGI(TAG, "WiFi connected, syncing RTC via NTP...");
            sync_rtc_from_ntp();
            wifi_stop();
            ntp_state = NTP_DONE;
            ESP_LOGI(TAG, "NTP sync complete, WiFi stopped");
        } else if (ntp_state == NTP_CONNECTING && wifi_elapsed_ms >= WIFI_TIMEOUT_MS) {
            ESP_LOGW(TAG, "WiFi connection timeout, stopping WiFi");
            wifi_stop();
            ntp_state = NTP_DONE;
        }
    }
}

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

#define TAG "basic_watch"

#define AWAKE_TIME_MS       5000  /* 5 seconds awake before deep sleep */

#define I2C_MASTER_TIMEOUT_MS 1000

#define NVS_NAMESPACE "storage"
#define NVS_KEY_BOOT_COUNT "boot_count"

#define WAKEUP_GPIO GPIO_NUM_0

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
    lv_label_set_text(lbl_status, "Deep Sleep - Press BOOT to wake");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 50);
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

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Deep Sleep Boot Example");
    ESP_LOGI(TAG, "========================================");

    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %s", reset_reason == ESP_RST_POWERON   ? "POWERON" :
                                       reset_reason == ESP_RST_EXT       ? "EXT" :
                                       reset_reason == ESP_RST_SW        ? "SW" :
                                       reset_reason == ESP_RST_PANIC     ? "PANIC" :
                                       reset_reason == ESP_RST_INT_WDT   ? "INT_WDT" :
                                       reset_reason == ESP_RST_TASK_WDT  ? "TASK_WDT" :
                                       reset_reason == ESP_RST_WDT       ? "WDT" :
                                       reset_reason == ESP_RST_DEEPSLEEP ? "DEEPSLEEP" :
                                       reset_reason == ESP_RST_BROWNOUT  ? "BROWNOUT" :
                                       reset_reason == ESP_RST_SDIO      ? "SDIO" :
                                       reset_reason == ESP_RST_USB       ? "USB" :
                                       reset_reason == ESP_RST_JTAG      ? "JTAG" :
                                       reset_reason == ESP_RST_EFUSE     ? "EFUSE" :
                                       reset_reason == ESP_RST_PWR_GLITCH? "PWR_GLITCH" :
                                       reset_reason == ESP_RST_CPU_LOCKUP? "CPU_LOCKUP" : "UNKNOWN");

    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wakeup cause: %s (0x%x)", wakeup_cause_to_str(wakeup_cause), wakeup_cause);

    /* Set Brazil timezone (BRT = UTC-3, no DST since 2019) */
    setenv("TZ", "BRT3", 1);
    tzset();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
    } else {
        int32_t boot_count = 0;
        nvs_get_i32(nvs_handle, NVS_KEY_BOOT_COUNT, &boot_count);
        boot_count++;
        ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, NVS_KEY_BOOT_COUNT, boot_count));
        ESP_ERROR_CHECK(nvs_commit(nvs_handle));
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Boot count: %ld", (long)boot_count);
    }

    /* Initialize display — this also initializes the BSP I2C bus on GPIO 14/15 */
    bsp_display_start();
    ESP_LOGI(TAG, "Display initialized");

    /* Add AXP2101 PMU to the BSP's existing I2C bus (avoids GPIO 14/15 conflict) */
    ESP_ERROR_CHECK(i2c_init_pmu());
    ESP_LOGI(TAG, "PMU I2C device added");

    /* Initialize PCF85063 RTC */
    rtc_cfg.i2c_bus = bsp_i2c_get_handle();
    esp_err_t rtc_ret = pcf85063_init(&rtc_cfg);
    if (rtc_ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 init failed: %s", esp_err_to_name(rtc_ret));
    } else {
        ESP_LOGI(TAG, "PCF85063 RTC initialized");
    }

    /* Create LVGL UI and show initial time */
    bsp_display_lock(0);
    create_ui();
    update_rtc_display();
    bsp_display_unlock();

    /* AXP2101 battery status */
    XPowersPMU PMU;
    if (!PMU.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte)) {
        ESP_LOGE(TAG, "AXP2101 init failed");
    } else {
        ESP_LOGI(TAG, "AXP2101 initialized successfully");

        PMU.disableTSPinMeasure();
        PMU.enableBattVoltageMeasure();
        PMU.enableVbusVoltageMeasure();
        PMU.enableSystemVoltageMeasure();
        PMU.enableTemperatureMeasure();

        uint16_t batt_mv = PMU.getBattVoltage();
        uint8_t batt_pct = PMU.getBatteryPercent();
        bool vbus_present = PMU.isVbusIn();
        bool charging = PMU.isCharging();
        uint16_t vbus_mv = PMU.getVbusVoltage();
        uint16_t sys_mv = PMU.getSystemVoltage();

        ESP_LOGI(TAG, "Battery: %u mV, %u%%", batt_mv, batt_pct);
        ESP_LOGI(TAG, "VBUS: %s (%u mV)", vbus_present ? "present" : "absent", vbus_mv);
        ESP_LOGI(TAG, "System: %u mV", sys_mv);
        ESP_LOGI(TAG, "Charging: %s", charging ? "yes" : "no");

        PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        PMU.clearIrqStatus();
    }

    /* Check if NTP sync is needed */
    pcf85063_time_t rtc_now;
    bool rtc_valid = (pcf85063_get_time(&rtc_cfg, &rtc_now) == ESP_OK);

    bool need_sync = false;
    if (!rtc_valid || rtc_now.year < 2026) {
        need_sync = true;
        ESP_LOGI(TAG, "RTC time invalid, needs NTP sync");
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
                ESP_LOGI(TAG, "Last NTP sync was > %d days ago, re-syncing", NTP_RESYNC_DAYS);
            } else {
                ESP_LOGI(TAG, "NTP synced recently, skipping");
            }
        } else {
            need_sync = true;
            ESP_LOGI(TAG, "No NTP sync record found");
        }
    }

    if (need_sync) {
        ntp_state = NTP_NEEDED;
    } else {
        ntp_state = NTP_NONE;
    }

    /* Start non-blocking WiFi if needed */
    if (ntp_state == NTP_NEEDED) {
        wifi_start();
    }

    /* Awake loop: update display every 1s, sync NTP when WiFi connects, hard exit at 5s */
    ESP_LOGI(TAG, "Awake for %d ms before deep sleep...", AWAKE_TIME_MS);
    int awake_elapsed = 0;
    int last_display_update = 0;

    while (awake_elapsed < AWAKE_TIME_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        awake_elapsed += 250;

        /* Update display every 1 second */
        if (awake_elapsed - last_display_update >= 1000) {
            last_display_update = awake_elapsed;
            bsp_display_lock(0);
            update_rtc_display();
            bsp_display_unlock();
        }

        /* If WiFi just connected, sync NTP */
        if (ntp_state == NTP_SYNCING) {
            ESP_LOGI(TAG, "WiFi connected, syncing RTC via NTP...");
            sync_rtc_from_ntp();
            wifi_stop();
            ntp_state = NTP_DONE;
            ESP_LOGI(TAG, "NTP sync complete, WiFi stopped");
        }
    }

    /* Cleanup WiFi if still active (timeout) */
    if (wifi_inited) {
        ESP_LOGW(TAG, "Stopping WiFi (no connection or still in progress)");
        wifi_stop();
    }

    /* Configure wakeup sources and enter deep sleep */
    /* Only EXT0 (BOOT button) wakeup — no timer */

    /* Configure GPIO0 as input with pull-up and read its level */
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = BIT64(WAKEUP_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_cfg));

    int gpio0_level = gpio_get_level(WAKEUP_GPIO);
    ESP_LOGI(TAG, "GPIO0 level before sleep: %d (%s)", gpio0_level,
             gpio0_level == 0 ? "LOW -> will wake immediately!" : "HIGH -> OK, needs press");

    /* EXT0 wakeup: RTC-controlled GPIO, survives deep sleep more reliably */
    ESP_LOGI(TAG, "Configuring GPIO0 as EXT0 wakeup (low-level trigger)");
    esp_sleep_enable_ext0_wakeup(WAKEUP_GPIO, 0);

    /* Turn off display before deep sleep */
    ESP_LOGI(TAG, "Turning off display...");
    bsp_display_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clean(lv_screen_active());
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(100));
    bsp_display_backlight_off();

    ESP_LOGI(TAG, "Entering deep sleep. Press BOOT (GPIO0) to wake up.");
    ESP_LOGI(TAG, "========================================");

    fflush(stdout);
    esp_deep_sleep_start();
}

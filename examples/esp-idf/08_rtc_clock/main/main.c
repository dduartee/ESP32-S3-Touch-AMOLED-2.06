#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "pcf85063.h"

static const char *TAG = "rtc_clock";

static pcf85063_config_t rtc_cfg = {
    .address = 0x51,
};

static lv_obj_t *lbl_time;
static lv_obj_t *lbl_date;
static lv_obj_t *lbl_status;

static const char *weekday_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static void update_ui(const pcf85063_time_t *t) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->hour, t->minute, t->second);
    lv_label_set_text(lbl_time, buf);

    snprintf(buf, sizeof(buf), "%s %d %d", weekday_names[t->weekday], t->month, t->day);
    lv_label_set_text(lbl_date, buf);
}

static void rtc_timer_cb(void *arg) {
    pcf85063_time_t t;
    if (pcf85063_get_time(&rtc_cfg, &t) == ESP_OK) {
        bsp_display_lock(0);
        update_ui(&t);
        bsp_display_unlock();
    }
}

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
    lv_label_set_text(lbl_status, "PCF85063 RTC Clock");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 50);

    lv_obj_t *lbl_boot = lv_label_create(scr);
    lv_label_set_text(lbl_boot, "Time set at boot");
    lv_obj_set_style_text_font(lbl_boot, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_boot, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_align(lbl_boot, LV_ALIGN_BOTTOM_MID, 0, -20);
}

void app_main(void) {
    bsp_display_start();

    rtc_cfg.i2c_bus = bsp_i2c_get_handle();

    esp_err_t ret = pcf85063_init(&rtc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 init failed: %s", esp_err_to_name(ret));
    }

    pcf85063_time_t init_time = {
        .year = 2026, .month = 7, .day = 17,
        .hour = 12, .minute = 0, .second = 0,
    };
    init_time.weekday = calc_weekday(init_time.year, init_time.month, init_time.day);
    pcf85063_set_time(&rtc_cfg, &init_time);

    bsp_display_lock(0);
    create_ui();
    bsp_display_unlock();

    const esp_timer_create_args_t rtc_timer_args = {
        .callback = rtc_timer_cb,
        .name = "rtc_1hz",
    };
    esp_timer_handle_t rtc_timer;
    esp_timer_create(&rtc_timer_args, &rtc_timer);
    esp_timer_start_periodic(rtc_timer, 1000000);

    ESP_LOGI(TAG, "RTC clock started");
}

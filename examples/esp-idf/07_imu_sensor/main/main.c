#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#ifdef M_PI
#undef M_PI
#endif
#include "qmi8658.h"

static const char *TAG = "imu_sensor";

#define IMU_TASK_STACK_SIZE 8192
#define IMU_TASK_PRIORITY   3
#define IMU_READ_DELAY_MS   50
#define CHART_POINTS        60
#define ANGLE_FILTER_ALPHA  0.1f

static lv_obj_t *label_accel_x;
static lv_obj_t *label_accel_y;
static lv_obj_t *label_accel_z;
static lv_obj_t *label_gyro_x;
static lv_obj_t *label_gyro_y;
static lv_obj_t *label_gyro_z;
static lv_obj_t *label_roll;
static lv_obj_t *label_pitch;
static lv_chart_series_t *chart_series;
static lv_obj_t *chart;

static float filtered_roll = 0.0f;
static float filtered_pitch = 0.0f;
static uint16_t chart_point_idx = 0;

static lv_color_t color_red   = { .blue = 0, .green = 0, .red = 255 };
static lv_color_t color_green = { .blue = 0, .green = 200, .red = 0 };
static lv_color_t color_blue  = { .blue = 255, .green = 0, .red = 0 };
static lv_color_t color_white = { .blue = 255, .green = 255, .red = 255 };

static void create_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "IMU Sensor Dashboard");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, color_white, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *section_accel = lv_label_create(scr);
    lv_label_set_text(section_accel, "ACCELEROMETER");
    lv_obj_set_style_text_font(section_accel, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(section_accel, color_white, 0);
    lv_obj_align(section_accel, LV_ALIGN_TOP_LEFT, 10, 50);

    label_accel_x = lv_label_create(scr);
    lv_label_set_text(label_accel_x, "X: 0.00 m/s2");
    lv_obj_set_style_text_font(label_accel_x, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_accel_x, color_red, 0);
    lv_obj_align(label_accel_x, LV_ALIGN_TOP_LEFT, 10, 75);

    label_accel_y = lv_label_create(scr);
    lv_label_set_text(label_accel_y, "Y: 0.00 m/s2");
    lv_obj_set_style_text_font(label_accel_y, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_accel_y, color_green, 0);
    lv_obj_align(label_accel_y, LV_ALIGN_TOP_LEFT, 10, 95);

    label_accel_z = lv_label_create(scr);
    lv_label_set_text(label_accel_z, "Z: 0.00 m/s2");
    lv_obj_set_style_text_font(label_accel_z, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_accel_z, color_blue, 0);
    lv_obj_align(label_accel_z, LV_ALIGN_TOP_LEFT, 10, 115);

    lv_obj_t *section_gyro = lv_label_create(scr);
    lv_label_set_text(section_gyro, "GYROSCOPE");
    lv_obj_set_style_text_font(section_gyro, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(section_gyro, color_white, 0);
    lv_obj_align(section_gyro, LV_ALIGN_TOP_LEFT, 10, 155);

    label_gyro_x = lv_label_create(scr);
    lv_label_set_text(label_gyro_x, "X: 0.00 rad/s");
    lv_obj_set_style_text_font(label_gyro_x, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_gyro_x, color_red, 0);
    lv_obj_align(label_gyro_x, LV_ALIGN_TOP_LEFT, 10, 180);

    label_gyro_y = lv_label_create(scr);
    lv_label_set_text(label_gyro_y, "Y: 0.00 rad/s");
    lv_obj_set_style_text_font(label_gyro_y, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_gyro_y, color_green, 0);
    lv_obj_align(label_gyro_y, LV_ALIGN_TOP_LEFT, 10, 200);

    label_gyro_z = lv_label_create(scr);
    lv_label_set_text(label_gyro_z, "Z: 0.00 rad/s");
    lv_obj_set_style_text_font(label_gyro_z, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_gyro_z, color_blue, 0);
    lv_obj_align(label_gyro_z, LV_ALIGN_TOP_LEFT, 10, 220);

    lv_obj_t *section_angle = lv_label_create(scr);
    lv_label_set_text(section_angle, "ANGLES");
    lv_obj_set_style_text_font(section_angle, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(section_angle, color_white, 0);
    lv_obj_align(section_angle, LV_ALIGN_TOP_LEFT, 10, 260);

    label_roll = lv_label_create(scr);
    lv_label_set_text(label_roll, "Roll:   0.0");
    lv_obj_set_style_text_font(label_roll, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_roll, color_white, 0);
    lv_obj_align(label_roll, LV_ALIGN_TOP_LEFT, 10, 285);

    label_pitch = lv_label_create(scr);
    lv_label_set_text(label_pitch, "Pitch:  0.0");
    lv_obj_set_style_text_font(label_pitch, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_pitch, color_white, 0);
    lv_obj_align(label_pitch, LV_ALIGN_TOP_LEFT, 10, 310);

    chart = lv_chart_create(scr);
    lv_obj_set_size(chart, 380, 170);
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 10, 340);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, CHART_POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -20, 20);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);

    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x333333), 0);

    chart_series = lv_chart_add_series(chart, color_white, LV_CHART_AXIS_PRIMARY_Y);
}

static void imu_update_task(void *arg)
{
    qmi8658_dev_t *dev = (qmi8658_dev_t *)arg;
    qmi8658_data_t data;

    while (1) {
        bool ready = false;
        esp_err_t ret = qmi8658_is_data_ready(dev, &ready);
        if (ret != ESP_OK || !ready) {
            vTaskDelay(pdMS_TO_TICKS(IMU_READ_DELAY_MS));
            continue;
        }

        ret = qmi8658_read_sensor_data(dev, &data);
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(IMU_READ_DELAY_MS));
            continue;
        }

        float accel_x = data.accelX;
        float accel_y = data.accelY;
        float accel_z = data.accelZ;
        float gyro_x  = data.gyroX;
        float gyro_y  = data.gyroY;
        float gyro_z  = data.gyroZ;

        float roll_raw  = atan2f(accel_y, accel_z) * (180.0f / M_PI);
        float pitch_raw = atan2f(-accel_x, sqrtf(accel_y * accel_y + accel_z * accel_z)) * (180.0f / M_PI);

        filtered_roll  = ANGLE_FILTER_ALPHA * roll_raw  + (1.0f - ANGLE_FILTER_ALPHA) * filtered_roll;
        filtered_pitch = ANGLE_FILTER_ALPHA * pitch_raw + (1.0f - ANGLE_FILTER_ALPHA) * filtered_pitch;

        float magnitude = sqrtf(accel_x * accel_x + accel_y * accel_y + accel_z * accel_z);

        bsp_display_lock(pdMS_TO_TICKS(100));

        lv_label_set_text_fmt(label_accel_x, "X: %6.2f m/s2", accel_x);
        lv_label_set_text_fmt(label_accel_y, "Y: %6.2f m/s2", accel_y);
        lv_label_set_text_fmt(label_accel_z, "Z: %6.2f m/s2", accel_z);
        lv_label_set_text_fmt(label_gyro_x,  "X: %6.2f rad/s", gyro_x);
        lv_label_set_text_fmt(label_gyro_y,  "Y: %6.2f rad/s", gyro_y);
        lv_label_set_text_fmt(label_gyro_z,  "Z: %6.2f rad/s", gyro_z);
        lv_label_set_text_fmt(label_roll,    "Roll:  %6.1f", filtered_roll);
        lv_label_set_text_fmt(label_pitch,   "Pitch: %6.1f", filtered_pitch);

        lv_chart_set_next_value(chart, chart_series, (lv_coord_t)magnitude);
        lv_chart_refresh(chart);

        bsp_display_unlock();

        vTaskDelay(pdMS_TO_TICKS(IMU_READ_DELAY_MS));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    lv_display_t *disp = bsp_display_start();
    if (disp) {
        bsp_display_backlight_on();
    }

    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
    qmi8658_dev_t *dev = malloc(sizeof(qmi8658_dev_t));
    ESP_ERROR_CHECK(qmi8658_init(dev, bus_handle, QMI8658_ADDRESS_HIGH));

    qmi8658_set_accel_range(dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(dev, QMI8658_ACCEL_ODR_500HZ);
    qmi8658_set_accel_unit_mps2(dev, true);

    qmi8658_set_gyro_range(dev, QMI8658_GYRO_RANGE_512DPS);
    qmi8658_set_gyro_odr(dev, QMI8658_GYRO_ODR_500HZ);
    qmi8658_set_gyro_unit_rads(dev, true);

    qmi8658_write_register(dev, QMI8658_CTRL5, 0x03);

    bsp_display_lock(pdMS_TO_TICKS(200));
    create_ui();
    bsp_display_unlock();

    ESP_LOGI(TAG, "IMU sensor dashboard initialized");

    xTaskCreatePinnedToCore(
        imu_update_task,
        "imu_update",
        IMU_TASK_STACK_SIZE,
        dev,
        IMU_TASK_PRIORITY,
        NULL,
        1
    );
}

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bt_nus.h"
#include "power_mgmt.h"

static const char *TAG = "ble_lvgl";

#define BLE_RX_QUEUE_SIZE   10
#define BLE_RX_MAX_LEN      512
#define SLEEP_TIMEOUT_S     10

typedef struct {
    size_t len;
    uint8_t data[BLE_RX_MAX_LEN + 1];
} ble_rx_msg_t;

static QueueHandle_t ble_rx_queue = NULL;

static void ble_rx_callback(const uint8_t *data, size_t len)
{
    if (len > BLE_RX_MAX_LEN) {
        len = BLE_RX_MAX_LEN;
    }
    ble_rx_msg_t msg;
    msg.len = len;
    memcpy(msg.data, data, len);
    xQueueSend(ble_rx_queue, &msg, 0);
}

static void display_task(void *pvParameters)
{
    bool sleeping = false;
    uint32_t idle_secs = 0;

    vTaskDelay(pdMS_TO_TICKS(500));

    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BLE + Sleep");
    lv_obj_set_style_text_color(title, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *conn_label = lv_label_create(scr);
    lv_label_set_text(conn_label, "Aguardando BLE...");
    lv_obj_set_style_text_color(conn_label, lv_color_hex(0xFF4444), 0);
    lv_obj_set_style_text_font(conn_label, &lv_font_montserrat_16, 0);
    lv_obj_align(conn_label, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *rx_label = lv_label_create(scr);
    lv_label_set_text(rx_label, "---");
    lv_obj_set_style_text_color(rx_label, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(rx_label, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(rx_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(rx_label, lv_pct(88));
    lv_obj_align(rx_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Envie texto pelo BLE\nusando um app NUS");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_t *sleep_label = lv_label_create(scr);
    lv_label_set_text(sleep_label, "");
    lv_obj_set_style_text_color(sleep_label, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_text_font(sleep_label, &lv_font_montserrat_16, 0);
    lv_obj_align(sleep_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    bsp_display_unlock();

    ble_rx_msg_t msg;
    bool prev_connected = false;

    while (1) {
        bool connected = bt_nus_is_connected();

        if (connected != prev_connected) {
            prev_connected = connected;
            if (connected && sleeping) {
                display_sleep_exit();
                sleeping = false;
                idle_secs = 0;
            }
            bsp_display_lock(0);
            lv_label_set_text(conn_label,
                              connected ? "Conectado" : "Aguardando BLE...");
            lv_obj_set_style_text_color(conn_label,
                connected ? lv_color_hex(0x00FF88) : lv_color_hex(0xFF4444), 0);
            bsp_display_unlock();
        }

        if (xQueueReceive(ble_rx_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            msg.data[msg.len] = 0;
            ESP_LOGI(TAG, "BLE RX (%d bytes): %.*s",
                     msg.len, msg.len, (const char *)msg.data);

            if (sleeping) {
                display_sleep_exit();
                sleeping = false;
            }

            bsp_display_lock(0);
            lv_label_set_text(rx_label, (const char *)msg.data);
            bsp_display_unlock();
            idle_secs = 0;
        } else {
            if (!sleeping) {
                idle_secs++;
            }
        }

        if (!sleeping && !connected) {
            bsp_display_lock(0);
            char buf[32];
            snprintf(buf, sizeof(buf), "Sleep in %lu s",
                     (unsigned long)(SLEEP_TIMEOUT_S - idle_secs));
            lv_label_set_text(sleep_label, buf);
            bsp_display_unlock();
        }

        if (!sleeping && !connected && idle_secs >= SLEEP_TIMEOUT_S) {
            ESP_LOGI(TAG, "Idle timeout -- entering display sleep");

            bsp_display_lock(0);
            lv_label_set_text(sleep_label, "Display sleeping...");
            lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);
            bsp_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(150));

            display_sleep_enter();
            sleeping = true;
            idle_secs = 0;

            ESP_LOGI(TAG, "Display asleep. Auto light sleep active. BLE stays alive.");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BLE NUS + LVGL + Auto Light Sleep starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }

    ESP_ERROR_CHECK(display_sleep_init(disp));

    ble_rx_queue = xQueueCreate(BLE_RX_QUEUE_SIZE, sizeof(ble_rx_msg_t));
    if (ble_rx_queue == NULL) {
        ESP_LOGE(TAG, "failed to create BLE RX queue");
        return;
    }

    bt_nus_set_rx_callback(ble_rx_callback);
    bt_nus_init();

    ESP_ERROR_CHECK(pm_configure_auto_light_sleep());

    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 2, NULL, 1);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

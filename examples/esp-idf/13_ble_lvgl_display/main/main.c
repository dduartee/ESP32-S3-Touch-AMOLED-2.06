#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bt_nus.h"

static const char *TAG = "ble_lvgl";

#define BLE_RX_QUEUE_SIZE 10
#define BLE_RX_MAX_LEN   512

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
    (void)pvParameters;

    vTaskDelay(pdMS_TO_TICKS(500));

    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BLE NUS Receiver");
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

    bsp_display_unlock();

    ble_rx_msg_t msg;
    bool prev_connected = false;

    while (1) {
        bool connected = bt_nus_is_connected();

        if (connected != prev_connected) {
            prev_connected = connected;
            bsp_display_lock(0);
            lv_label_set_text(conn_label,
                              connected ? "Conectado" : "Aguardando BLE...");
            lv_obj_set_style_text_color(conn_label,
                connected ? lv_color_hex(0x00FF88) : lv_color_hex(0xFF4444), 0);
            bsp_display_unlock();
        }

        if (xQueueReceive(ble_rx_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            msg.data[msg.len] = 0;
            ESP_LOGI(TAG, "BLE RX (%d bytes): %s", msg.len, (const char *)msg.data);
            bsp_display_lock(0);
            lv_label_set_text(rx_label, (const char *)msg.data);
            bsp_display_unlock();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BLE NUS + LVGL Receiver starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    bsp_display_start();

    ble_rx_queue = xQueueCreate(BLE_RX_QUEUE_SIZE, sizeof(ble_rx_msg_t));
    if (ble_rx_queue == NULL) {
        ESP_LOGE(TAG, "failed to create BLE RX queue");
        return;
    }

    bt_nus_set_rx_callback(ble_rx_callback);
    bt_nus_init();

    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 2, NULL, 1);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

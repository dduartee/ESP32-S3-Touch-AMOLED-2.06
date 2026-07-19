#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "bt_nus.h"

static const char *TAG = "ble_nus";

static void status_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t count = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        count++;

        char buf[64];
        int len = snprintf(buf, sizeof(buf),
                           ">NUS alive count=%lu\n", (unsigned long)count);

        if (bt_nus_is_connected()) {
            bt_nus_send((const uint8_t *)buf, len);
        }
        printf("%s", buf);
    }
}

static void button_task(void *pvParameters)
{
    (void)pvParameters;

    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&btn_cfg);

    int debounce = 0;
    bool prev_raw = true;
    bool pressed = false;

    while (1) {
        bool raw = gpio_get_level(GPIO_NUM_0);

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

                    const char *msg = ">cmd:btn_press\n";
                    if (bt_nus_is_connected()) {
                        bt_nus_send((const uint8_t *)msg, strlen(msg));
                    }
                    printf("%s", msg);

                } else if (raw) {
                    pressed = false;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "BLE NUS example starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    bt_nus_init();

    xTaskCreatePinnedToCore(status_task, "status", 3072, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(button_task, "button", 2560, NULL, 1, NULL, 1);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

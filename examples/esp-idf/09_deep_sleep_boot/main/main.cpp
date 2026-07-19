/**
 * @file main.cpp
 * @brief Minimal deep sleep boot example for ESP32-S3-Touch-AMOLED-2.06
 *
 * Demonstrates:
 *   - Cold boot / wakeup detection
 *   - NVS boot counter across deep sleep cycles
 *   - AXP2101 PMIC battery status read via I2C (GPIO14=SCL, GPIO15=SDA)
 *   - 5 second awake window
 *   - Deep sleep with two wakeup sources:
 *       - EXT0 (GPIO0 / BOOT button, active-low)
 *       - 60 second timer
 *
 * No display, no LVGL, no WiFi/NTP — console output only.
 */

#include <stdio.h>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static const char *TAG = "deep_sleep";

#define AWAKE_TIME_MS       5000        /* 5 seconds awake before deep sleep */
#define TIMER_WAKEUP_US     60000000    /* 60 second timer wakeup */
#define I2C_MASTER_TIMEOUT_MS 1000

/* I2C pins for AXP2101 PMIC */
#define I2C_SCL_IO          GPIO_NUM_14
#define I2C_SDA_IO          GPIO_NUM_15

/* NVS namespace and keys */
#define NVS_NAMESPACE       "storage"
#define NVS_KEY_BOOT_COUNT  "boot_count"

/* PMU device handle for I2C callbacks */
static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t pmu_dev_handle = NULL;

/* ---------------------------------------------------------------------------
 * PMU I2C read / write callbacks
 * -------------------------------------------------------------------------*/
static int pmu_register_read(uint8_t devAddr, uint8_t regAddr,
                             uint8_t *data, uint8_t len)
{
    esp_err_t ret = i2c_master_transmit_receive(
        pmu_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU read failed (reg 0x%02x): %s",
                 regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

static int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr,
                                   uint8_t *data, uint8_t len)
{
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;

    buffer[0] = regAddr;
    memcpy(buffer + 1, data, len);

    esp_err_t ret = i2c_master_transmit(
        pmu_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU write failed (reg 0x%02x): %s",
                 regAddr, esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * I2C master bus initialisation (GPIO14=SCL, GPIO15=SDA, 400 kHz)
 * -------------------------------------------------------------------------*/
static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_SLAVE_ADDRESS,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };

    err = i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &pmu_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Initialise and read AXP2101 PMU battery status
 * -------------------------------------------------------------------------*/
static void axp2101_init_and_log(void)
{
    XPowersPMU PMU;

    if (!PMU.begin(AXP2101_SLAVE_ADDRESS,
                   pmu_register_read,
                   pmu_register_write_byte))
    {
        ESP_LOGE(TAG, "AXP2101 init failed");
        return;
    }

    ESP_LOGI(TAG, "AXP2101 PMU initialised");

    /* Configure measurement channels */
    PMU.disableTSPinMeasure();
    PMU.enableBattVoltageMeasure();
    PMU.enableVbusVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.enableTemperatureMeasure();

    /* Disable all interrupts (we poll in this example) */
    PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    PMU.clearIrqStatus();

    /* Read and log battery status */
    uint16_t batt_mv   = PMU.getBattVoltage();
    uint8_t  batt_pct  = PMU.getBatteryPercent();
    bool     vbus_in   = PMU.isVbusIn();
    bool     charging  = PMU.isCharging();
    uint16_t vbus_mv   = PMU.getVbusVoltage();
    uint16_t sys_mv    = PMU.getSystemVoltage();

    ESP_LOGI(TAG, "Battery:  %u mV, %u%%", batt_mv, batt_pct);
    ESP_LOGI(TAG, "VBUS:     %s (%u mV)",  vbus_in ? "present" : "absent", vbus_mv);
    ESP_LOGI(TAG, "System:   %u mV",       sys_mv);
    ESP_LOGI(TAG, "Charging: %s",          charging ? "yes" : "no");
}

/* ---------------------------------------------------------------------------
 * app_main
 * -------------------------------------------------------------------------*/
extern "C" void app_main(void)
{
    /* ---- Banner ---- */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Deep Sleep Boot Example (Minimal)");
    ESP_LOGI(TAG, "========================================");

    /* ---- Wakeup cause ---- */
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    switch (wakeup_cause) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            ESP_LOGI(TAG, "Wakeup: cold boot (power-on / reset)");
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
            ESP_LOGI(TAG, "Wakeup: EXT0 — BOOT button (GPIO0 low)");
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "Wakeup: 60 s timer expired");
            break;
        default:
            ESP_LOGI(TAG, "Wakeup: other (cause=%d)", wakeup_cause);
            break;
    }

    /* ---- NVS boot counter ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t nvs_handle;
    int32_t boot_count = 0;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        nvs_get_i32(nvs_handle, NVS_KEY_BOOT_COUNT, &boot_count);
        boot_count++;
        ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, NVS_KEY_BOOT_COUNT, boot_count));
        ESP_ERROR_CHECK(nvs_commit(nvs_handle));
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Boot count: %ld", (long)boot_count);

    /* ---- I2C + AXP2101 battery read ---- */
    ret = i2c_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C master bus ready (SCL=GPIO14, SDA=GPIO15)");
        axp2101_init_and_log();
    } else {
        ESP_LOGE(TAG, "I2C init failed — skipping AXP2101 read");
    }

    /* ---- Awake window ---- */
    ESP_LOGI(TAG, "Awake for %u ms...", AWAKE_TIME_MS);
    vTaskDelay(pdMS_TO_TICKS(AWAKE_TIME_MS));

    /* ---- Configure deep sleep wakeup sources ---- */

    /* GPIO0 as input with pull-up (BOOT button, active-low) */
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = BIT64(GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_cfg));

    int gpio0_level = gpio_get_level(GPIO_NUM_0);
    ESP_LOGI(TAG, "GPIO0 level: %d (%s)", gpio0_level,
             gpio0_level == 0 ? "LOW — will wake immediately!" : "HIGH");

    /* EXT0 wakeup: BOOT button pressed (low level) */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0));

    /* Timer wakeup: 60 seconds */
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(TIMER_WAKEUP_US));

    /* ---- Enter deep sleep ---- */
    ESP_LOGI(TAG, "Entering deep sleep. Press BOOT to wake, or wait 60 s.");
    ESP_LOGI(TAG, "========================================");

    fflush(stdout);
    esp_deep_sleep_start();

    /* Never reached */
}

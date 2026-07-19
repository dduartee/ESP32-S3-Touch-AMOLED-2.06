#include "pcf85063.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "pcf85063";

#define PCF85063_ADDR           0x51
#define PCF85063_CTRL1_REG      0x00
#define PCF85063_CTRL1_STOP     (1 << 5)
#define PCF85063_CTRL1_12H      (1 << 1)
#define PCF85063_RAM_REG        0x03
#define PCF85063_SEC_REG        0x04
#define PCF85063_YEAR_REG       0x0A

#define I2C_TIMEOUT_MS          1000

static uint8_t dec2bcd(uint8_t dec) {
    return (uint8_t)((dec / 10) << 4 | (dec % 10));
}

static uint8_t bcd2dec(uint8_t bcd) {
    return (uint8_t)(((bcd >> 4) * 10) + (bcd & 0x0F));
}

static esp_err_t pcf85063_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, 2, I2C_TIMEOUT_MS);
}

uint8_t calc_weekday(uint16_t year, uint8_t month, uint8_t day) {
    if (month < 3) {
        month += 12;
        year--;
    }
    return (uint8_t)((day + 2 * month + 3 * (month + 1) / 5 + year + year / 4 - year / 100 + year / 400 + 1) % 7);
}

esp_err_t pcf85063_init(const pcf85063_config_t *cfg) {
    i2c_master_dev_handle_t dev;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(cfg->i2c_bus, &dev_cfg, &dev), TAG, "add device");

    uint8_t val;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev, &(uint8_t){PCF85063_RAM_REG}, 1, &val, 1, I2C_TIMEOUT_MS), TAG, "read RAM");
    uint8_t saved = val;

    val |= (1 << 7);
    ESP_RETURN_ON_ERROR(pcf85063_write_reg(dev, PCF85063_RAM_REG, val), TAG, "write RAM bit7");
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev, &(uint8_t){PCF85063_RAM_REG}, 1, &val, 1, I2C_TIMEOUT_MS), TAG, "read back RAM");

    if (!(val & (1 << 7))) {
        ESP_LOGE(TAG, "RAM R/W test failed - not PCF85063?");
        i2c_master_bus_rm_device(dev);
        return ESP_ERR_NOT_FOUND;
    }

    val &= ~(1 << 7);
    ESP_RETURN_ON_ERROR(pcf85063_write_reg(dev, PCF85063_RAM_REG, val), TAG, "clear RAM bit7");

    if (saved != val) {
        ESP_RETURN_ON_ERROR(pcf85063_write_reg(dev, PCF85063_RAM_REG, saved), TAG, "restore RAM");
    }

    uint8_t ctrl1;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev, &(uint8_t){PCF85063_CTRL1_REG}, 1, &ctrl1, 1, I2C_TIMEOUT_MS), TAG, "read CTRL1");

    ctrl1 &= ~PCF85063_CTRL1_STOP;
    ESP_RETURN_ON_ERROR(pcf85063_write_reg(dev, PCF85063_CTRL1_REG, ctrl1), TAG, "clear STOP");

    ctrl1 &= ~PCF85063_CTRL1_12H;
    ESP_RETURN_ON_ERROR(pcf85063_write_reg(dev, PCF85063_CTRL1_REG, ctrl1), TAG, "set 24H");

    ESP_LOGI(TAG, "init OK, CTRL1=0x%02X", ctrl1);

    i2c_master_bus_rm_device(dev);
    return ESP_OK;
}

esp_err_t pcf85063_set_time(const pcf85063_config_t *cfg, const pcf85063_time_t *time) {
    i2c_master_dev_handle_t dev;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(cfg->i2c_bus, &dev_cfg, &dev), TAG, "add device");

    uint8_t buf[7];
    buf[0] = dec2bcd(time->second) & 0x7F;
    buf[1] = dec2bcd(time->minute);
    buf[2] = dec2bcd(time->hour);
    buf[3] = dec2bcd(time->day);
    buf[4] = time->weekday & 0x07;
    buf[5] = dec2bcd(time->month);
    buf[6] = dec2bcd(time->year % 100);

    uint8_t payload[8];
    payload[0] = PCF85063_SEC_REG;
    memcpy(&payload[1], buf, 7);

    esp_err_t ret = i2c_master_transmit(dev, payload, 8, I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);
    ESP_RETURN_ON_ERROR(ret, TAG, "write time");

    ESP_LOGI(TAG, "set time: %04d-%02d-%02d %02d:%02d:%02d",
             time->year, time->month, time->day, time->hour, time->minute, time->second);
    return ESP_OK;
}

esp_err_t pcf85063_get_time(const pcf85063_config_t *cfg, pcf85063_time_t *time) {
    i2c_master_dev_handle_t dev;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(cfg->i2c_bus, &dev_cfg, &dev), TAG, "add device");

    uint8_t buf[7];
    esp_err_t ret = i2c_master_transmit_receive(dev, &(uint8_t){PCF85063_SEC_REG}, 1, buf, 7, I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);
    ESP_RETURN_ON_ERROR(ret, TAG, "read time");

    time->second = bcd2dec(buf[0] & 0x7F);
    time->minute = bcd2dec(buf[1] & 0x7F);
    time->hour   = bcd2dec(buf[2] & 0x3F);
    time->day    = bcd2dec(buf[3] & 0x3F);
    time->weekday = buf[4] & 0x07;
    time->month  = bcd2dec(buf[5] & 0x1F);
    time->year   = bcd2dec(buf[6]) + 2000;

    return ESP_OK;
}

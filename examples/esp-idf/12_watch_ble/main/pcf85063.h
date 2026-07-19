#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    uint8_t address;
} pcf85063_config_t;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
} pcf85063_time_t;

esp_err_t pcf85063_init(const pcf85063_config_t *cfg);
esp_err_t pcf85063_set_time(const pcf85063_config_t *cfg, const pcf85063_time_t *time);
esp_err_t pcf85063_get_time(const pcf85063_config_t *cfg, pcf85063_time_t *time);
uint8_t calc_weekday(uint16_t year, uint8_t month, uint8_t day);

#ifdef __cplusplus
}
#endif

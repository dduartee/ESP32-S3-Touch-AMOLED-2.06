#ifndef BT_NUS_H
#define BT_NUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bt_nus_rx_cb_t)(const uint8_t *data, size_t len);

void bt_nus_init(void);
bool bt_nus_is_connected(void);
int  bt_nus_send(const uint8_t *data, size_t len);
void bt_nus_set_rx_callback(bt_nus_rx_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* BT_NUS_H */

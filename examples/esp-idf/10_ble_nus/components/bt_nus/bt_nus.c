/*
 * SPDX-FileCopyrightText: 2025 track-kinesis
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * BLE Nordic UART Service (NUS) peripheral using NimBLE.
 *
 * Implements the standard NUS profile:
 *   - Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   - TX Characteristic (ESP32 -> client): 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (Notify)
 *   - RX Characteristic (client -> ESP32): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (Write)
 */

#include "bt_nus.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "bt_nus";

static _Atomic bool       connected          = false;
static _Atomic bool       subscribed         = false;
static          uint16_t  nus_tx_val_handle  = 0;
static          uint16_t  conn_handle        = 0;
static          uint8_t   own_addr_type;

static int  bt_nus_gap_event(struct ble_gap_event *event, void *arg);
static int  bt_nus_gatt_handler(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);
static void bt_nus_advertise(void);
static int  gatt_svr_init(void);

static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
);

static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
);

static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
);

static const struct ble_gatt_svc_def nus_gatt_defs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &nus_tx_uuid.u,
                .access_cb  = bt_nus_gatt_handler,
                .val_handle = &nus_tx_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid      = &nus_rx_uuid.u,
                .access_cb = bt_nus_gatt_handler,
                .flags     = BLE_GATT_CHR_F_WRITE,
            },
            { 0 }
        },
    },
    { 0 }
};

static void
gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "registered NUS service handle=%d", ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG, "registered characteristic def_handle=%d val_handle=%d",
                 ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;
    default:
        break;
    }
}

static int
bt_nus_gatt_handler(uint16_t conn_handle, uint16_t attr_handle,
                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        size_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0) {
            char *buf = malloc(len + 1);
            if (buf) {
                memcpy(buf, ctxt->om->om_data, len);
                buf[len] = 0;
                ESP_LOGI(TAG, "NUS RX: %s", buf);
                printf("[NUS RX] %s\n", buf);
                free(buf);
            }
        }
    }

    return 0;
}

static int
bt_nus_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE client connected");
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                conn_handle = event->connect.conn_handle;
                connected = true;
            }
        } else {
            ESP_LOGW(TAG, "BLE connection failed, status=%d", event->connect.status);
            bt_nus_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE client disconnected, reason=%d", event->disconnect.reason);
        connected    = false;
        subscribed   = false;
        bt_nus_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe event conn=%x attr=%x cur_notify=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        subscribed = event->subscribe.cur_notify ? true : false;
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update conn=%x mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void
bt_nus_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = &nus_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv rsp set fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, bt_nus_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
}

static void
bt_nus_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

static void
bt_nus_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed: %d", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0]);
    }

    bt_nus_advertise();
}

static void
bt_nus_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void
bt_nus_init(void)
{
    esp_err_t ret;

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_att_set_preferred_mtu(256);

    ble_hs_cfg.reset_cb        = bt_nus_on_reset;
    ble_hs_cfg.sync_cb         = bt_nus_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap       = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc           = 0;

    int rc = gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt_svr_init failed: %d", rc);
        return;
    }

    rc = ble_svc_gap_device_name_set("ESP32-S3-NUS");
    if (rc != 0) {
        ESP_LOGE(TAG, "device name set failed: %d", rc);
    }

    nimble_port_freertos_init(bt_nus_host_task);

    ESP_LOGI(TAG, "bt_nus initialized");
}

bool
bt_nus_is_connected(void)
{
    return connected && subscribed;
}

int
bt_nus_send(const uint8_t *data, size_t len)
{
    if (!subscribed) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *txom = ble_hs_mbuf_from_flat(data, len);
    if (txom == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(conn_handle, nus_tx_val_handle, txom);
    if (rc != 0) {
        os_mbuf_free_chain(txom);
        return rc;
    }

    return (int)len;
}

static int
gatt_svr_init(void)
{
    int rc;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(nus_gatt_defs);
    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_add_svcs(nus_gatt_defs);
    return rc;
}

#ifndef TETHER_BLE_H
#define TETHER_BLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_serial_slave_link/essl_sdio.h"

#include "tether_sdio_proto.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    uint8_t idx;
    uint8_t mac[TETHER_MAC_LEN];
    int8_t  rssi;
    char    name[TETHER_SHOW_NAME_MAX + 1];
} tether_dev_t;

typedef struct {
    uint8_t mac[TETHER_MAC_LEN];
    int8_t  rssi;
    char    name[TETHER_CONN_NAME_MAX + 1];
} tether_conn_t;

typedef struct {
    uint8_t idx;
    uint8_t mac[TETHER_MAC_LEN];
    char    name[TETHER_READ_ADV_NAME_MAX + 1];
    uint8_t data[TETHER_READ_ADV_DATA_MAX];
    uint8_t data_len;
} tether_adv_detail_t;

typedef enum {
    TETHER_BLE_EVT_PAIR_COMPLETE,
    TETHER_BLE_EVT_PEER_DISCONN,
    TETHER_BLE_EVT_PAIR_REMOVE,
} tether_ble_evt_kind_t;

typedef enum {
    TETHER_CENT_TO_PERIPHERAL = 1,
    TETHER_PERIPHERAL_TO_CENT = 2,
} tether_conn_type_t;

typedef struct {
    tether_ble_evt_kind_t kind;
    union {
        struct { 
            uint8_t mac[TETHER_MAC_LEN]; 
            bool success;
            tether_conn_type_t connection_type;
        } pair_complete;
        struct {
            uint8_t mac[TETHER_MAC_LEN];
            uint16_t reason; bool intentional;
        } peer_disconn;
        struct {
            uint8_t mac[TETHER_MAC_LEN];
        } pair_remove;
    };
} tether_ble_event_t;

typedef void (*tether_ble_event_cb_t)(const tether_ble_event_t *evt, void *user);


esp_err_t tether_ble_init(essl_handle_t handle);

void tether_ble_set_event_cb(tether_ble_event_cb_t cb, void *user);


esp_err_t tether_ble_show(tether_dev_t *out, uint8_t cap, uint8_t *count_out,
                          uint32_t timeout_ms);

esp_err_t tether_ble_pair(uint8_t index, uint32_t timeout_ms);

esp_err_t tether_ble_pair_by_mac(const uint8_t *mac, uint32_t timeout_ms);

esp_err_t tether_ble_read_adv(uint8_t index, tether_adv_detail_t *out,
                              uint32_t timeout_ms);

esp_err_t tether_ble_connected(tether_conn_t *out, uint8_t cap, uint8_t *count_out,
                               uint32_t timeout_ms);

esp_err_t tether_ble_disconnect(uint8_t index, uint32_t timeout_ms);

esp_err_t tether_ble_remove(uint8_t index, const uint8_t mac[TETHER_MAC_LEN],
                            uint32_t timeout_ms);

/* Erase the C6 bond for the Nth connected peer (OP_REMOVE) without touching the P4 store.
 * For callers that manage the store themselves (e.g. the UI on its own thread).           */
esp_err_t tether_ble_unpair(uint8_t index, uint32_t timeout_ms);

esp_err_t tether_ble_detected(tether_scan_report_t *out, uint32_t timeout_ms);


#ifdef __cplusplus
}
#endif

#endif /* TETHER_BLE_H */

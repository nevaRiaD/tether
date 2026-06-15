#ifndef BLE_TETHER_H
#define BLE_TETHER_H

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_central.h"

/* USER INCLUDE */
#include "tether_slave_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BLE */
#define BLE_PEER_NAME           		"Holy-IOT"  // prefix C6 auto-connects to (central role)
#define BLE_LOCAL_NAME          		"Tether"    // name C6 broadcasts itself (peripheral role); kept in sync with ext_adv_pattern_1 in ble_tether.c
#define BLE_PEER_MAX_NUM        		(MYNEWT_VAL(BLE_MAX_CONNECTIONS) - 1)
#define BLE_PREF_EVT_LEN_MS     		(5)
#define BLE_PREF_CONN_ITVL_MS   		(BLE_PEER_MAX_NUM * BLE_PREF_EVT_LEN_MS)

/**
 * Initializes the NimBLE host stack, registers BLE callbacks, configures GATT services, and
 * spawns the host task. Call once during system startup before any other BLE operation.
 */
void ble_tether_init(void);

/**
 * Initiate a connection to the advertiser at the given address. Used by the console pair flow.
 * Returns 0 on success, non-zero on failure (e.g. max connections reached, NimBLE error).
 */
int ble_tether_connect_addr(const ble_addr_t *peer_addr);

/**
 * Start the GAP discovery procedure (active scan). No-op if already scanning. The snapshot in
 * ble_pair fills as advertising packets arrive. Pair with `ble_tether_stop_scan` and a delay
 * to run a discrete scan window.
 */
void ble_tether_start_scan(void);

/**
 * Start a time-limited scan that auto-stops after duration_ms.
 */
void ble_tether_start_scan_timed(uint32_t duration_ms);

/**
 * Cancel the GAP discovery procedure. No-op if no scan is active.
 */
void ble_tether_stop_scan(void);

/**
 * Returns the number of currently active BLE connections (central + peripheral).
 */
int ble_tether_conn_count(void);


#ifdef __cplusplus
}
#endif

#endif // BLE_TETHER_H

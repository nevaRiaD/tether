/****************************************************************
 * FILENAME: ble_rssi_buff.h
 * DESCRIPTION: Provides filter functions to detect close and stationary devices (using RSSI)
 * AUTHOR: Jaycee Alipio
 * DATE: 2026-04-24
 ****************************************************************/

#ifndef BLE_RSSI_BUFF_H
#define BLE_RSSI_BUFF_H

#include <stdint.h>
#include "host/ble_hs.h"
#include "esp_central.h"
#include "tether_slave_cfg.h"
#include <stdbool.h>

typedef struct __attribute__((packed)) {
	uint8_t			count;
	uint8_t			entries[MAX_CONNECTED_DEVICES][6];
} scan_report_t;

/* ========== RSSI BUFFER FUNCTIONS ========== */
bool peer_rssi_buff_full(const struct peer *peer);
bool peer_rssi_buff_empty(const struct peer *peer);
int peer_rssi_buff_push(struct peer *peer, int8_t rssi);
int peer_rssi_buff_pop(struct peer *peer, int8_t *rssi);
int peer_rssi_comp_var(const struct peer *peer, uint16_t *var_out);

/* peer_traverse_all callback: classifies a connected peer as "close and non-stationary"
 * and, if so, appends its MAC to the scan_report_t passed via arg. Returns 0 to continue
 * traversal. */
int ble_rssi_move_close(const struct peer *peer, void *arg);

/* ========== SCAN REPORT ========== */

/* Copy the most recent close-and-moving snapshot into *out. The BLE polling task refreshes
 * this every BLE_POLL_INTERVAL_MS; callers (e.g. the SDIO OP_SCAN handler) pull it on demand.
 * Returns 0 on success, or 1 if the polling task has not run yet (*out is zeroed). */
int ble_get_latest_scan(scan_report_t *out);

#endif // BLE_RSSI_BUFF_H
#ifndef BLE_PAIR_H
#define BLE_PAIR_H

#include "ble_rssi_buff.h"
#include "ble_pair_utils.h"

/* ========== Snapshot recording / accessors ========== */

/* Record an advertiser (with optional parsed name and manufacturer data) into snapshot. */
void ble_pair_record_adv(const ble_addr_t *addr, const uint8_t *name, uint8_t name_len,
                         const uint8_t *mfr_data, uint8_t mfr_data_len, int8_t rssi);

/* Copy the current sorted advertising snapshot (count + per-entry mac/handles) into *out. */
int ble_show_adv_devices(adv_snapshot_t *out);

/* Reset the snapshot to empty. Used before each fresh scan window. */
void ble_pair_reset_snapshot(void);

/* Remove entries from the snapshot that haven't advertised within max_age_ms.
 * Call periodically (e.g. from ble_collectdevice_task) to evict out-of-range devices. */
void ble_pair_purge_stale(uint32_t max_age_ms);


/* ========== Per-device accessors (shared by console and SDIO transports) ========== */

/* Copy device name at `idx` into `out` (NUL-terminated). Empty string if device has no name.
 * Returns 0 on success, 1 if idx is out of range. */
int ble_pair_get_name(uint8_t idx, char *out, uint8_t cap);

/* Copy RSSI for device at `idx` into *out. Returns 0/1. */
int ble_pair_get_rssi(uint8_t idx, int8_t *out);

/* Copy manufacturer data for device at `idx` into `out` (up to `cap` bytes); *out_len receives
 * the actual length written. Returns 0/1. */
int ble_pair_get_mfr(uint8_t idx, uint8_t *out, uint8_t cap, uint8_t *out_len);

/* Clear cached mfr data for device at `idx` so a subsequent advertisement reads as fresh. */
void ble_pair_clear_mfr(uint8_t idx);

/* Copy the cached display name for a given MAC into `out` ("(unknown)" if not in snapshot). */
void ble_pair_find_name_by_mac(const uint8_t *mac, char *out, uint8_t cap);

/* Resolve a 6-byte MAC to its position in the current adv snapshot.
 * Returns 0 on match (writes to *out_idx), 1 if not present. */
int ble_pair_find_snapshot_index_by_mac(const uint8_t mac[6], uint8_t *out_idx);

/* Resolve a name (case-insensitive, exact) to its position in the current adv snapshot.
 * Returns 0 on a unique match, 1 if no device has that name, 2 if multiple devices share it. */
int ble_pair_find_snapshot_index_by_name(const char *name, uint8_t *out_idx);

/* Same as ble_pair_find_snapshot_index_by_mac but against the live connected-peer list
 * (in the same order `connected` prints). Returns 0 on match, 1 if no connected peer matches. */
int ble_pair_find_connected_index_by_mac(const uint8_t mac[6], uint8_t *out_idx);

/* Same as ble_pair_find_snapshot_index_by_name but against the live connected-peer list.
 * Returns 0/1/2 with the same meaning. The name comes from each peer's cached snapshot entry. */
int ble_pair_find_connected_index_by_name(const char *name, uint8_t *out_idx);


/* Fill *out with MACs of advertised devices whose name matches "Tether Tag <number>".
 * Returns 0 on success. */
int ble_get_tether_tags(scan_report_t *out);



/* ========== BLE actions ========== */

/* Connect to the device at `index` in the snapshot; send paired MAC to SDIO.            */
int ble_pair_device(uint8_t index);

/* Connect to the device matching `mac` in the snapshot. Looks up addr_type from snapshot. */
int ble_pair_device_mac(const uint8_t *mac);

/* Erase the bond for the Nth connected peer (delete keys, drop link) and notify SDIO of
 * the removal via a PAIR_REMOVE event. For console-initiated removes.                    */
int ble_remove_device(uint8_t index);

/* Erase the bond for the Nth connected peer (delete keys, drop link) WITHOUT emitting a
 * PAIR_REMOVE event. For host-initiated removes (SDIO OP_REMOVE), where the P4 already
 * holds the MAC and purges its own store. Returns 0 on success, 1 otherwise.            */
int ble_unpair_device(uint8_t index);

/* Disconnect the Nth currently-connected peer (same ordering as the `connected` command).
 * Returns 0 on success, 1 if there is no peer at that index or the terminate call fails. */
int ble_disconnect_device(uint8_t index);

#endif // BLE_PAIR_H

#include "ble_accel.h"
#include "ble_pair.h"
#include "ble_rssi_buff.h"
#include "esp_central.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BLE_ACCEL_TAG";

/**
 * @brief Read the accel tag's cached motion flag.
 *
 * The flag is refreshed by the NimBLE host task in BLE_GAP_EVENT_NOTIFY_RX each
 * time the tag sends a motion notification (see ble_tether.c). Here we just read
 * the cached byte — no GATT round-trip.
 *
 * @param peer  Connected accelerometer peer.
 * @return      1 if the tag last reported motion, 0 otherwise.
*/
static int ble_accel_parse(const struct peer *peer)
{
    return peer->accel_moving ? 1 : 0;
}

int ble_accel_move_close(const struct peer *peer, void *arg)
{
    scan_report_t *scan_report = (scan_report_t *)arg;
    if (peer->device_type != DEVICE_TYPE_ACCEL) {
        return 0; /* skip this peer */
    }
    
    int8_t rssi = 0;
    if (ble_gap_conn_rssi(peer->conn_handle, &rssi) != 0) {
		return 0; /* skip this peer, keep traversing */
	}

    bool is_close = (rssi > RSSI_THRESHOLD_ACCEL);
    bool is_moving = ble_accel_parse(peer);

#ifdef T_BLE_LOG_ACCEL_MOVE_CLOSE
    ESP_LOGI(TAG, "TODO: Insert Info here");
#else // Store Accelerometer Devices to scan reports
    if (is_close && is_moving && (scan_report->count < MAX_CONNECTED_DEVICES)) {
        /* Creates connection handle to store mac-id */
		struct ble_gap_conn_desc desc;
		if (ble_gap_conn_find(peer->conn_handle, &desc) != 0) {
			ESP_LOGI(TAG, "Cannot find connection handle");
			return 0;
		}

		uint8_t i = scan_report->count;
		memcpy(scan_report->entries[i], desc.peer_id_addr.val, 6);
		scan_report->count++;
	}
#endif
    return 0;
}
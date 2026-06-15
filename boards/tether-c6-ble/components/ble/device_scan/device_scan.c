#include "device_scan.h"
#include "ble_rssi_buff.h"
#include "ble_accel.h"
#include "ble_pair.h"
#include "esp_central.h"
#include "tether_slave_cfg.h"

static const char *TAG = "DEVICE_SCAN";

static scan_report_t     s_latest_scan;
static SemaphoreHandle_t s_scan_mtx;

void ble_collectdevice_task(void *param)
{
    if (s_scan_mtx == NULL) {
        s_scan_mtx = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "BLE Display Rssi Task Started");

    while (1) {
        ble_pair_purge_stale(MAX_ADVERTISEMENT_INTERVAL);

        /* Both passes append into one report; each callback gates on
         * detected.count < MAX_CONNECTED_DEVICES, so the shared count caps the
         * combined RSSI + accel set without a separate budget. */
        scan_report_t detected = {0};

        /* RSSI tags: close-and-moving inferred from connected RSSI variance. */
        peer_traverse_all(ble_rssi_move_close, &detected);
        uint8_t rssi_count = detected.count;   /* entries [0, rssi_count) are RSSI */

        /* Accel tags: close-and-moving from the cached motion flag (GATT notify). */
        peer_traverse_all(ble_accel_move_close, &detected);
        /* entries [rssi_count, detected.count) are accelerometer */

#ifdef T_BLE_LOG_DEVICESCAN
        /* DEBUG: dump the detected set with per-device type (RSSI vs ACCEL). */
        ESP_LOGI(TAG, "scan: %u detected (%u rssi, %u accel)",
                 detected.count, rssi_count, (uint8_t)(detected.count - rssi_count));
        for (uint8_t i = 0; i < detected.count; i++) {
            const uint8_t *m = detected.entries[i];   /* little-endian; print MSB-first */
            ESP_LOGI(TAG, "  [%u] %-5s %02x:%02x:%02x:%02x:%02x:%02x",
                     i, (i < rssi_count) ? "RSSI" : "ACCEL",
                     m[5], m[4], m[3], m[2], m[1], m[0]);
        }
#endif
        /* publish the fresh snapshot; the SDIO OP_DETECT handler pulls it on demand */
        xSemaphoreTake(s_scan_mtx, portMAX_DELAY);
        s_latest_scan = detected;
        xSemaphoreGive(s_scan_mtx);
        
        vTaskDelay(pdMS_TO_TICKS(BLE_POLL_INTERVAL_MS));
    }
}

int ble_get_latest_scan(scan_report_t *out)
{
    if (s_scan_mtx == NULL) {
        memset(out, 0, sizeof(*out));
        return 1;
    }
    xSemaphoreTake(s_scan_mtx, portMAX_DELAY);
    *out = s_latest_scan;
    xSemaphoreGive(s_scan_mtx);
	return 0;
}

#include "device_scan.h"
#include "tether_ble.h"
#include "esp_log.h"
#include "tag.h"
#include "tether_config.h"
#include "tether_sdio_proto.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "device_scan";
static tether_scan_report_t prev_report;

/* Max time to wait for the C6's OP_DETECT response. The snapshot is pre-computed on the C6,
 * so the reply is prompt — this only needs to cover the SDIO round-trip. */
#define DEVICE_SCAN_TIMEOUT_MS  1000

bool User_Detected(void){
    uint16_t distance_mm;
    return get_tof_distance(&distance_mm) && (distance_mm != UINT16_MAX) && (distance_mm < TOF_THRESHOLD_MM);
}

/**
 * Pull the C6's current close-and-moving proximity snapshot and run the tag-tracking
 * algorithm against it. Intended to be called when the ToF sensor detects a user at the
 * threshold: issues OP_DETECT, then fuses the returned device list with the local store.
 *
 * Blocks the calling task for the SDIO round-trip (a few ms). Call from a dedicated task,
 * not from the tether-ble event callback.
 */
void device_scan_poll(void)
{
    tether_scan_report_t curr_report = {0};
    esp_err_t err = tether_ble_detected(&curr_report, DEVICE_SCAN_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "detect failed: %s", esp_err_to_name(err));
        return;
    }

#ifdef T_PRINTSCAN_TEST_MODE
    ESP_LOGI(TAG, "scan_report: %u device(s)", report.count);
    for (uint8_t i = 0; i < report.count; i++) {
        const uint8_t *m = report.entries[i];
        ESP_LOGI(TAG, "  [%u] %02X:%02X:%02X:%02X:%02X:%02X",
                 i, m[5], m[4], m[3], m[2], m[1], m[0]);
    }
#else
    // detects changes between current iteration and previous to prevent spam
    if (memcmp(&curr_report, &prev_report, sizeof curr_report) == 0) {
        // current iteration = prev iteration
        return;
    }
    prev_report = curr_report;

    if (curr_report.count > 0) {
        UserDetectionResult results[MAX_USERS];
        int user_count = 0;
        tag_algorithm(curr_report.entries, curr_report.count,
                      results, MAX_USERS, &user_count);

        for (int u = 0; u < user_count; u++) {
            UserDetectionResult *r = &results[u];
            ESP_LOGI(TAG, "User %" PRIu32 " present, %d missing",
                     r->user_id, r->missing_count);
            if (r->missing_count > 0) {
                for (int m = 0; m < r->missing_count; m++) {
                    ESP_LOGI(TAG, "  missing: %s (id=%" PRIu32 ")",
                             r->missing[m]->tag_name, r->missing[m]->tag_id);
                }
                // TODO: send per-user alert to UI
            }
        }
        if (user_count == 0) {
            ESP_LOGI(TAG, "Detected devices but none in store");
        }
    }
    else {
        ESP_LOGI(TAG, "User Detected but no Devices");
    }
#endif
}


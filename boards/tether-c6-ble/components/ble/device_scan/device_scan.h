#ifndef DEVICE_SCAN_H
#define DEVICE_SCAN_H

#include "ble_rssi_buff.h"

/**
 * @brief Collects detected devices and stores in s_latest_scan
 *
 *  Purges stale advertisements, and then collects both rssi
 *  and accelerometer devices to store with semaphore to prevent
 *  race conditions.
 *
 * @param param: null value
 */
void ble_collectdevice_task(void *param);

/**
 * @brief Gets latest scan report of detected devices
 *
 * @param out: null value
 * @return 1 if fail, 0 if success
 */
int ble_get_latest_scan(scan_report_t *out);

#endif // DEVICE_SCAN_H
#ifndef BLE_ACCEL_H
#define BLE_ACCEL_H

#include "esp_central.h"

/**
 * @brief
 *
 * @param peer:
 * @param arg:
 * @return If accelerometer device is moving
*/
int ble_accel_move_close(const struct peer *peer, void *arg);

#endif // BLE_ACCEL_H
#ifndef GATT_SVR_H
#define GATT_SVR_H

/**
 * @brief Register the GATT services and characteristics with the NimBLE host.
 *
 * Counts and adds the server's service definitions and initializes the
 * built-in GAP and GATT services. Call once during BLE host startup, after
 * NimBLE is initialized but before advertising begins.
 *
 * @return 0 on success; a NimBLE host error code otherwise.
 */
int tether_gatt_svr_init(void);

#endif // GATT_SVR_H
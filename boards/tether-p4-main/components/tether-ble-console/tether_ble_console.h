#ifndef TETHER_BLE_CONSOLE_H
#define TETHER_BLE_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up an esp_console REPL on the P4's primary console (UART or USB-Serial-JTAG) and
 * register the BLE command set that mirrors the C6 console: show, pair, read_adv, connected,
 * disconnect, help. Each command shells out to a tether_ble_* call. Invoke once during
 * startup, after tether_ble_init. */
void tether_ble_console_init(void);

#ifdef __cplusplus
}
#endif

#endif /* TETHER_BLE_CONSOLE_H */

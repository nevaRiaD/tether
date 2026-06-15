#include "ble_pair_console.h"
#include "ble_pair.h"
#include "ble_pair_utils.h"
#include "ble_tether.h"
#include "esp_central.h"

#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "c6_console";

/* Which list a `[mac|id|name] <value>` selector resolves against. */
typedef enum {
    TARGET_SRC_SNAPSHOT,    /* `show` list — used by pair / read_adv      */
    TARGET_SRC_CONNECTED,   /* `connected` list — used by disconnect / remove */
} target_source_t;


/* ========== Forward declarations ========== */

/* Parse a printed MAC "AA:BB:CC:DD:EE:FF" (high-byte first, as `show`/`connected` print) into
 * internal little-endian storage. Returns 0 on success, 1 on malformed input. */
static int parse_printed_mac(const char *s, uint8_t out[6]);

/* Parse `[mac|id|name] <value>` from argv and resolve it to an index in the requested source.
 * Prints a diagnostic on any error. Returns 0 on success, non-zero on failure. */
static int resolve_target(int argc, char **argv, target_source_t src, uint8_t *out_idx);

/* peer_traverse_all callback: print one connected peer (idx, name, MAC, RSSI) and bump count. */
static int list_peer_cb(const struct peer *peer, void *arg);


/* console commands */

/* `show` handler: clear the snapshot, scan for ADV_SCAN_WINDOW_MS, then dump the sorted list.  */
static int cmd_show(int argc, char **argv);

/* `connected` handler: enumerate currently connected peers via peer_traverse_all.              */
static int cmd_connected(int argc, char **argv);

/* `read_adv` handler: resolve a snapshot target and print its name + raw manufacturer data.    */
static int cmd_read_adv(int argc, char **argv);

/* `pair` handler: resolve a snapshot target and connect via ble_pair_device.                   */
static int cmd_pair(int argc, char **argv);

/* `remove` handler: disconnect device and remove from p4 database                              */
static int cmd_remove(int argc, char **argv);

/* `disconnect` handler: resolve a connected-list target and terminate it.                      */
static int cmd_disconnect(int argc, char **argv);

/* `help` handler: print the list of available commands.                                        */
static int cmd_help(int argc, char **argv);



/* ========== Function definitions ========== */

static int parse_printed_mac(const char *s, uint8_t out[6])
{
    unsigned b[6];
    char tail = 0;
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail);
    if (n != 6) return 1;
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) return 1;
        out[5 - i] = (uint8_t)b[i];
    }
    return 0;
}

static int resolve_target(int argc, char **argv, target_source_t src, uint8_t *out_idx)
{
    const char *cmd = (argc > 0) ? argv[0] : "<cmd>";
    if (argc < 3) {
        printf("usage: %s [mac|id|name] <value>\n", cmd);
        return 1;
    }
    const char *kind  = argv[1];
    const char *value = argv[2];

    if (strcmp(kind, "id") == 0) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || n < 0 || n > 255) {
            printf("invalid id: %s\n", value);
            return 1;
        }
        *out_idx = (uint8_t)n;
        return 0;
    }

    if (strcmp(kind, "mac") == 0) {
        uint8_t mac[6];
        if (parse_printed_mac(value, mac) != 0) {
            printf("invalid mac: %s (want AA:BB:CC:DD:EE:FF)\n", value);
            return 1;
        }
        int rc = (src == TARGET_SRC_SNAPSHOT)
                     ? ble_pair_find_snapshot_index_by_mac(mac, out_idx)
                     : ble_pair_find_connected_index_by_mac(mac, out_idx);
        if (rc != 0) {
            const char *where = (src == TARGET_SRC_SNAPSHOT) ? "snapshot" : "connected list";
            printf("no device with mac %s in %s\n", value, where);
            return 1;
        }
        return 0;
    }

    if (strcmp(kind, "name") == 0) {
        int rc = (src == TARGET_SRC_SNAPSHOT)
                     ? ble_pair_find_snapshot_index_by_name(value, out_idx)
                     : ble_pair_find_connected_index_by_name(value, out_idx);
        if (rc == 1) {
            const char *where = (src == TARGET_SRC_SNAPSHOT) ? "snapshot" : "connected list";
            printf("no device named \"%s\" in %s\n", value, where);
            return 1;
        }
        if (rc == 2) {
            printf("multiple devices named \"%s\" — disambiguate with `mac` or `id`\n", value);
            return 1;
        }
        return 0;
    }

    printf("unknown selector \"%s\" (use mac, id, or name)\n", kind);
    return 1;
}

/**
 * peer_traverse_all callback that prints one currently connected peer. Looks up the live
 * conn handle for RSSI, resolves the cached display name from the snapshot, and increments
 * the caller's counter so the outer command can report a total.
 *
 * @param peer  NimBLE peer record being visited.
 * @param arg   uint8_t* counter to bump for each peer printed.
 * @return      Always 0 so peer_traverse_all visits the next peer.
 */
static int list_peer_cb(const struct peer *peer, void *arg)
{
    uint8_t *count = (uint8_t *)arg;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(peer->conn_handle, &desc) != 0) {
        return 0;
    }
    int8_t rssi = 0;
    ble_gap_conn_rssi(peer->conn_handle, &rssi);
    const uint8_t *m = desc.peer_id_addr.val;
    char name[ADV_NAME_MAX];
    ble_pair_find_name_by_mac(m, name, sizeof name);
    printf("  [%u] %s  %02X:%02X:%02X:%02X:%02X:%02X  %d dBm\n",
           *count, name, m[5], m[4], m[3], m[2], m[1], m[0], rssi);
    (*count)++;
    return 0;
}

// ========== CONSOLE COMMANDS ==========

/**
 * `show` console command. Resets the advertising snapshot, blocks for ADV_SCAN_WINDOW_MS
 * while NimBLE accumulates fresh adv reports, then prints each entry from the sorted
 * snapshot with its MAC, name (or "<unknown>"), and last-seen RSSI.
 *
 * @param argc  Unused (no arguments).
 * @param argv  Unused.
 * @return      0 on success, 1 if the snapshot is unavailable.
 */
static int cmd_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ble_pair_reset_snapshot();
    printf("Scanning for %d ms...\n", ADV_SCAN_WINDOW_MS);
    ble_tether_start_scan();
    vTaskDelay(pdMS_TO_TICKS(ADV_SCAN_WINDOW_MS));
    ble_tether_stop_scan();

    adv_snapshot_t snap;
    if (ble_show_adv_devices(&snap) != 0) {
        printf("snapshot unavailable\n");
        return 1;
    }

    printf("Found %u devices:\n", snap.count);
    for (uint8_t i = 0; i < snap.count; i++) {
        const uint8_t *m = snap.entries[i];
        char   name[ADV_NAME_MAX];
        int8_t rssi = 0;
        ble_pair_get_name(i, name, sizeof name);
        ble_pair_get_rssi(i, &rssi);
        const char *disp = name[0] ? name : "<unknown>";
        printf("  [%u] %02X:%02X:%02X:%02X:%02X:%02X  %-32s  %d dBm\n",
               i, m[5], m[4], m[3], m[2], m[1], m[0], disp, rssi);
    }
    return 0;
}

/**
 * `connected` console command. Walks every currently connected NimBLE peer via
 * peer_traverse_all, printing each through list_peer_cb, and reports the total count.
 *
 * @param argc  Unused.
 * @param argv  Unused.
 * @return      Always 0.
 */
static int cmd_connected(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t count = 0;
    peer_traverse_all(list_peer_cb, &count);
    if (count == 0) {
        printf("No connected devices.\n");
    } else {
        printf("Connected: %u device(s).\n", count);
    }
    return 0;
}

/**
 * `read_adv [mac|id|name] <value>` console command. Resolves the target against the adv
 * snapshot, clears its cached manufacturer data, then blocks for ADV_SINGLE_SHOT_SCAN_MS
 * so the next advertisement from that device repopulates the slot. Prints the device's
 * name and the raw mfr-data bytes, or notes that no fresh advertisement was seen.
 *
 * @param argc  Argument count from the REPL.
 * @param argv  Argument vector; argv[1] is the selector kind, argv[2] the value.
 * @return      0 on success, 1 on parse/lookup error or missing snapshot.
 */
static int cmd_read_adv(int argc, char **argv)
{
    uint8_t idx;
    if (resolve_target(argc, argv, TARGET_SRC_SNAPSHOT, &idx) != 0) return 1;

    adv_snapshot_t snap;
    if (ble_show_adv_devices(&snap) != 0 || snap.count == 0) {
        printf("No snapshot — run `show` first\n");
        return 1;
    }
    if (idx >= snap.count) {
        printf("index out of range (have %u devices — run `show` first)\n", snap.count);
        return 1;
    }

    /* Clear this device's mfr data so we can detect whether a fresh advertisement arrives. */
    ble_pair_clear_mfr(idx);

    printf("Scanning for device [%u] (%d ms)...\n", idx, ADV_SINGLE_SHOT_SCAN_MS);
    ble_tether_start_scan();
    vTaskDelay(pdMS_TO_TICKS(ADV_SINGLE_SHOT_SCAN_MS));
    ble_tether_stop_scan();

    const uint8_t *m = snap.entries[idx];
    char    name[ADV_NAME_MAX];
    uint8_t data[ADV_DATA_MAX];
    uint8_t dlen = 0;
    ble_pair_get_name(idx, name, sizeof name);
    ble_pair_get_mfr(idx, data, sizeof data, &dlen);

    const char *disp = name[0] ? name : "<unknown>";
    printf("Device [%u] %02X:%02X:%02X:%02X:%02X:%02X  %s\n",
           idx, m[5], m[4], m[3], m[2], m[1], m[0], disp);
    if (dlen == 0) {
        printf("  Mfr data: <device did not advertise during scan window>\n");
    } else {
        printf("  Mfr data (%u bytes):", dlen);
        for (uint8_t j = 0; j < dlen; j++) {
            printf(" %02X", data[j]);
        }
        printf("\n");
    }
    return 0;
}

/**
 * `pair [mac|id|name] <value>` console command. Resolves the target against the adv snapshot
 * and hands off to ble_pair_device() which uses the snapshot to initiate the BLE connection.
 *
 * @param argc  Argument count from the REPL.
 * @param argv  Argument vector; argv[1] is the selector kind, argv[2] the value.
 * @return      0 on success, 1 on parse/lookup error or BLE-side failure.
 */
static int cmd_pair(int argc, char **argv)
{
    uint8_t idx;
    if (resolve_target(argc, argv, TARGET_SRC_SNAPSHOT, &idx) != 0) return 1;
    return ble_pair_device(idx);
}

/**
 * `remove [mac|id|name] <value>` console command. Disconnects device from peer
 * connected list and communicates to P4 to remove device
 *
 * @param argc  Argument count from the REPL.
 * @param argv  Argument vector; argv[1] is the selector kind, argv[2] the value.
 * @return      0 on success, 1 on parse/lookup error or BLE-side failure.
 */
static int cmd_remove(int argc, char **argv)
{
    uint8_t idx;
    if (resolve_target(argc, argv, TARGET_SRC_CONNECTED, &idx) != 0) return 1;
    return ble_remove_device(idx);
}

/**
 * `disconnect [mac|id|name] <value>` console command. Resolves the target against the
 * connected-peer list and hands off to ble_disconnect_device(). The connected list — not
 * the `show` snapshot — is what gets matched, so this works for iOS/LE-Privacy devices
 * whose identity address differs from the address they advertised with.
 *
 * @param argc  Argument count from the REPL.
 * @param argv  Argument vector; argv[1] is the selector kind, argv[2] the value.
 * @return      0 on success, 1 on parse/lookup error or BLE-side failure.
 */
static int cmd_disconnect(int argc, char **argv)
{
    uint8_t idx;
    if (resolve_target(argc, argv, TARGET_SRC_CONNECTED, &idx) != 0) return 1;
    return ble_disconnect_device(idx);
}

/**
 * `help` console command. Prints a static description of each registered command.
 *
 * @param argc  Unused.
 * @param argv  Unused.
 * @return      Always 0.
 */
static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("\nCommands:\n");
    printf("  show                              Scan and list nearby BLE advertisers\n");
    printf("  connected                         List all currently connected devices with RSSI\n");
    printf("  read_adv [mac|id|name] <value>    Print name and raw mfr data for a snapshot device\n");
    printf("  pair [mac|id|name] <value>        Connect to a device in the last `show` snapshot\n");
    printf("  remove [mac|id|name] <value>      Disconnect device and remove from database\n");
    printf("  disconnect [mac|id|name] <value>  Terminate a currently connected device\n");
    printf("  help                              Show this list\n");
    printf("\nSelectors:\n");
    printf("  id    <n>                         Position in the `show` (pair/read_adv) or `connected` list\n");
    printf("  mac   AA:BB:CC:DD:EE:FF           MAC as printed by `show` / `connected`\n");
    printf("  name  <text>                      Case-insensitive exact name match\n");
    printf("\n");
    return 0;
}

/**
 * Bring up the esp_console REPL on the configured console hardware (UART or USB-Serial-JTAG)
 * and register every BLE-pair command. Call once during system startup after ble_tether_init.
 */
void ble_pair_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "tether-c6> ";
    repl_config.max_cmdline_length = 64;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
    #error "ble_pair_console_init: no supported console selected in sdkconfig (need UART_DEFAULT, UART_CUSTOM, or USB_SERIAL_JTAG)"
#endif
    // show command
    const esp_console_cmd_t show_cmd = {
        .command = "show",
        .help    = NULL,
        .func    = &cmd_show,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&show_cmd));

    // list connected command
    const esp_console_cmd_t list_connected_cmd = {
        .command = "connected",
        .help    = NULL,
        .func    = &cmd_connected,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&list_connected_cmd));

    // read advertisements command
    const esp_console_cmd_t read_adv_cmd = {
        .command = "read_adv",
        .help    = NULL,
        .hint    = "[mac|id|name] <value>",
        .func    = &cmd_read_adv,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&read_adv_cmd));

    // pair command
    const esp_console_cmd_t pair_cmd = {
        .command = "pair",
        .help    = NULL,
        .hint    = "[mac|id|name] <value>",
        .func    = &cmd_pair,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pair_cmd));

    // remove command
    const esp_console_cmd_t remove_cmd = {
        .command = "remove",
        .help    = NULL,
        .hint    = "[mac|id|name] <value>",
        .func    = &cmd_remove,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&remove_cmd));

    // disconnect command
    const esp_console_cmd_t disconnect_cmd = {
        .command = "disconnect",
        .help    = NULL,
        .hint    = "[mac|id|name] <value>",
        .func    = &cmd_disconnect,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&disconnect_cmd));

    // help command
    const esp_console_cmd_t help_cmd = {
        .command = "help",
        .help    = NULL,
        .func    = &cmd_help,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&help_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "C6 console ready: type `help` for commands");
}

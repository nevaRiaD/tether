/*
 * P4-side REPL that mirrors the C6 console commands. Each handler calls the matching
 * tether_ble_* function from the tether-ble component and prints the result in the same
 * shape as the C6 console, so users can drive either chip and see the same output style.
 *
 * The dispatched commands all block the calling task — the REPL task itself — until the
 * SDIO round trip completes or the per-command timeout fires. show is the slow one (5s+
 * for the C6's active scan window); the others return as soon as the C6 acks.
 */

#include "tether_ble_console.h"
#include "tether_ble.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>


/* ========== Config / state ========== */

#define TAG_CON                     "p4_console"

#define SHOW_TIMEOUT_MS             10000   /* must exceed C6 ADV_SCAN_WINDOW_MS (5s) plus slack */
#define PAIR_TIMEOUT_MS             5000
#define READ_ADV_TIMEOUT_MS         3000
#define CONNECTED_TIMEOUT_MS        3000
#define DISCONNECT_TIMEOUT_MS       3000

#define SHOW_CAP                    255         /* matches C6 MAX_ADVERTISED_DEVICES */
#define CONNECTED_CAP               16

/* Which list a `[mac|id|name] <value>` selector resolves against. Mirrors the C6 console. */
typedef enum {
    TARGET_SRC_SNAPSHOT,    /* `show` list — used by pair / read_adv      */
    TARGET_SRC_CONNECTED,   /* `connected` list — used by disconnect / remove */
} target_source_t;


/* ========== Forward declarations ========== */

/* Parse a printed MAC "AA:BB:CC:DD:EE:FF" (high-byte first, as `show`/`connected` print) into
 * internal little-endian storage. Returns 0 on success, 1 on malformed input. */
static int parse_printed_mac(const char *s, uint8_t out[6]);

/* Parse `[mac|id|name] <value>` from argv and resolve it to an index the C6 understands.
 * `id` passes straight through; `mac`/`name` fetch the requested list over SDIO (a fresh
 * scan for the snapshot) and search it locally. If `out_mac` is non-NULL, the resolved
 * device's MAC is also returned (which forces a list fetch even for `id`). Prints a
 * diagnostic on any error. Returns 0 on success, non-zero on failure. */
static int resolve_target(int argc, char **argv, target_source_t src,
                          uint8_t *out_idx, uint8_t *out_mac);


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

/**
 * Resolve a `[mac|id|name] <value>` selector to an index the C6 understands. Mirrors the C6
 * console's resolve_target, but the P4 keeps no local lists: it fetches the relevant list over
 * SDIO and searches it here. `id` is a pure index passthrough UNLESS out_mac is requested, in
 * which case the list is fetched to recover the MAC at that position.
 *
 *   - TARGET_SRC_SNAPSHOT (pair/read_adv): calls tether_ble_show, which triggers a fresh ~5s
 *     scan on the C6. The matched entry's `idx` is what we hand back, so it stays valid for
 *     the snapshot the C6 now holds. A device seen by a prior `show` may not reappear in this
 *     fresh scan — use `id` (without a MAC request) to avoid the re-scan.
 *   - TARGET_SRC_CONNECTED (disconnect/remove): calls tether_ble_connected; the matching
 *     entry's array position is the index, matching the order the C6's OP_DISCONNECT expects.
 *
 * @param argc     Argument count from the REPL.
 * @param argv     argv[1] is the selector kind, argv[2] the value.
 * @param src      Which C6 list to resolve against.
 * @param out_idx  Receives the resolved index on success.
 * @param out_mac  If non-NULL, receives the resolved device's 6-byte MAC (forces a list fetch).
 * @return         0 on success, non-zero on parse/lookup/transport error.
 */
static int resolve_target(int argc, char **argv, target_source_t src,
                          uint8_t *out_idx, uint8_t *out_mac)
{
    const char *cmd = (argc > 0) ? argv[0] : "<cmd>";
    if (argc < 3) {
        printf("usage: %s [mac|id|name] <value>\n", cmd);
        return 1;
    }
    const char *kind  = argv[1];
    const char *value = argv[2];

    const bool want_id   = (strcmp(kind, "id")   == 0);
    const bool want_mac  = (strcmp(kind, "mac")  == 0);
    const bool want_name = (strcmp(kind, "name") == 0);
    if (!want_id && !want_mac && !want_name) {
        printf("unknown selector \"%s\" (use mac, id, or name)\n", kind);
        return 1;
    }

    long    id_val = -1;
    uint8_t mac[6];
    if (want_id) {
        char *end = NULL;
        id_val = strtol(value, &end, 10);
        if (end == value || *end != '\0' || id_val < 0 || id_val > 255) {
            printf("invalid id: %s\n", value);
            return 1;
        }
        /* Pure index passthrough unless the caller also needs the MAC. */
        if (out_mac == NULL) {
            *out_idx = (uint8_t)id_val;
            return 0;
        }
    } else if (want_mac && parse_printed_mac(value, mac) != 0) {
        printf("invalid mac: %s (want AA:BB:CC:DD:EE:FF)\n", value);
        return 1;
    }

    const char    *where     = (src == TARGET_SRC_SNAPSHOT) ? "snapshot" : "connected list";
    int            found      = -1;
    const uint8_t *found_mac  = NULL;

    if (src == TARGET_SRC_SNAPSHOT) {
        static tether_dev_t devs[SHOW_CAP];  /* ~7.4 KB; static to keep it off the console stack */
        uint8_t n = 0;
        printf("Scanning to resolve %s (waiting up to %d ms)...\n", kind, SHOW_TIMEOUT_MS);
        esp_err_t r = tether_ble_show(devs, SHOW_CAP, &n, SHOW_TIMEOUT_MS);
        if (r != ESP_OK) {
            printf("scan failed: 0x%x\n", r);
            return 1;
        }
        for (uint8_t i = 0; i < n; i++) {
            bool match = want_id  ? (devs[i].idx == (uint8_t)id_val)
                       : want_mac ? (memcmp(devs[i].mac, mac, TETHER_MAC_LEN) == 0)
                                  : (strcasecmp(devs[i].name, value) == 0);
            if (!match) continue;
            if (found >= 0 && want_name) {
                printf("multiple devices named \"%s\" — disambiguate with `mac` or `id`\n", value);
                return 1;
            }
            found     = devs[i].idx;
            found_mac = devs[i].mac;
            if (want_mac || want_id) break;   /* mac/id are unique */
        }
    } else {
        tether_conn_t conns[CONNECTED_CAP];
        uint8_t n = 0;
        esp_err_t r = tether_ble_connected(conns, CONNECTED_CAP, &n, CONNECTED_TIMEOUT_MS);
        if (r != ESP_OK) {
            printf("connected query failed: 0x%x\n", r);
            return 1;
        }
        for (uint8_t i = 0; i < n; i++) {
            bool match = want_id  ? (i == (uint8_t)id_val)
                       : want_mac ? (memcmp(conns[i].mac, mac, TETHER_MAC_LEN) == 0)
                                  : (strcasecmp(conns[i].name, value) == 0);
            if (!match) continue;
            if (found >= 0 && want_name) {
                printf("multiple devices named \"%s\" — disambiguate with `mac` or `id`\n", value);
                return 1;
            }
            found     = i;   /* connected index is array position */
            found_mac = conns[i].mac;
            if (want_mac || want_id) break;
        }
    }

    if (found < 0) {
        if (want_id)       printf("no device with id %ld in %s\n", id_val, where);
        else if (want_mac) printf("no device with mac %s in %s\n", value, where);
        else               printf("no device named \"%s\" in %s\n", value, where);
        return 1;
    }
    *out_idx = (uint8_t)found;
    if (out_mac != NULL) {
        memcpy(out_mac, found_mac, TETHER_MAC_LEN);
    }
    return 0;
}

/**
 * `show` console command. Calls tether_ble_show, which blocks until the C6 has finished its
 * active scan window (about 5 seconds) and returned the device list. Prints each device with
 * MAC, name (or "<unknown>"), and last-seen RSSI.
 *
 * @param argc  Unused.
 * @param argv  Unused.
 * @return      0 on success, 1 on transport / timeout error.
 */
static int cmd_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static tether_dev_t devs[SHOW_CAP];  /* ~7.4 KB; static to keep it off the console stack */
    uint8_t             n = 0;

    printf("Scanning (waiting up to %d ms)...\n", SHOW_TIMEOUT_MS);
    esp_err_t r = tether_ble_show(devs, SHOW_CAP, &n, SHOW_TIMEOUT_MS);
    if (r != ESP_OK) {
        printf("show failed: 0x%x\n", r);
        return 1;
    }
    printf("Found %u devices:\n", n);
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *m = devs[i].mac;
        printf("  [%u] %02X:%02X:%02X:%02X:%02X:%02X  %-32s  %d dBm\n",
               devs[i].idx,
               m[5], m[4], m[3], m[2], m[1], m[0],
               devs[i].name[0] ? devs[i].name : "<unknown>",
               devs[i].rssi);
    }
    return 0;
}

/**
 * `connected` console command. Asks the C6 to enumerate currently connected peers and prints
 * the index (matching the order used by `disconnect`), name, MAC, and live RSSI for each.
 *
 * @param argc  Unused.
 * @param argv  Unused.
 * @return      0 on success, 1 on transport / timeout error.
 */
static int cmd_connected(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    tether_conn_t conns[CONNECTED_CAP];
    uint8_t       n = 0;
    esp_err_t     r = tether_ble_connected(conns, CONNECTED_CAP, &n, CONNECTED_TIMEOUT_MS);
    if (r != ESP_OK) {
        printf("connected failed: 0x%x\n", r);
        return 1;
    }
    if (n == 0) {
        printf("No connected devices.\n");
    } else {
        for (uint8_t i = 0; i < n; i++) {
            const uint8_t *m = conns[i].mac;
            printf("  [%u] %02X:%02X:%02X:%02X:%02X:%02X  %-32s  %d dBm\n",
                   i, m[5], m[4], m[3], m[2], m[1], m[0],
                   conns[i].name[0] ? conns[i].name : "<unknown>", conns[i].rssi);
        }
        printf("Connected: %u device(s).\n", n);
    }
    return 0;
}


/**
 * `read_adv [mac|id|name] <value>` console command. Resolves the target against the snapshot,
 * then returns the cached name + manufacturer data for that device. Unlike the C6 console
 * version, this does NOT trigger a fresh single-shot scan (the SDIO dispatcher returns whatever
 * the snapshot currently has) — though `mac`/`name` resolution does itself re-scan via `show`.
 *
 * @param argc  Argument count from the REPL.
 * @param argv  argv[1] is the selector kind, argv[2] the value.
 * @return      0 on success, 1 on parse/lookup/transport error.
 */
static int cmd_read_adv(int argc, char **argv)
{
    uint8_t idx;
    if (resolve_target(argc, argv, TARGET_SRC_SNAPSHOT, &idx, NULL) != 0) return 1;

    tether_adv_detail_t detail;
    esp_err_t r = tether_ble_read_adv(idx, &detail, READ_ADV_TIMEOUT_MS);
    if (r != ESP_OK) {
        printf("read_adv failed: 0x%x\n", r);
        return 1;
    }
    const uint8_t *m = detail.mac;
    printf("Device [%u] %02X:%02X:%02X:%02X:%02X:%02X  %s\n",
           detail.idx,
           m[5], m[4], m[3], m[2], m[1], m[0],
           detail.name[0] ? detail.name : "<unknown>");
    if (detail.data_len == 0) {
        printf("  Mfr data: <none>\n");
    } else {
        printf("  Mfr data (%u bytes):", detail.data_len);
        for (uint8_t j = 0; j < detail.data_len; j++) {
            printf(" %02X", detail.data[j]);
        }
        printf("\n");
    }
    return 0;
}

/**
 * `pair [mac|id|name] <value>` console command. Resolves the target against the snapshot and
 * calls tether_ble_pair. The C6's ACK comes back quickly; the actual BLE connection completes
 * asynchronously and lands as TETHER_BLE_EVT_PAIR_COMPLETE in the event callback.
 *
 * @param argc  Argument count from the REPL.
 * @param argv  argv[1] is the selector kind, argv[2] the value.
 * @return      0 on accepted request, 1 on parse/lookup/transport error.
 */
static int cmd_pair(int argc, char **argv)
{
    uint8_t idx;
    if (resolve_target(argc, argv, TARGET_SRC_SNAPSHOT, &idx, NULL) != 0) return 1;

    esp_err_t r = tether_ble_pair(idx, PAIR_TIMEOUT_MS);
    if (r != ESP_OK) {
        printf("pair failed: 0x%x\n", r);
        return 1;
    }
    printf("pair request acknowledged (wait for PAIR_COMPLETE event)\n");
    return 0;
}

/**
 * `remove [mac|id|name] <value>` console command. Resolves the target against the connected
 * list (matching the C6 console's `remove`), purges the tag from the P4's local store, then asks
 * the C6 to disconnect it. resolve_target hands back both the connected index (for the C6) and
 * the MAC (for the store), so no second connected fetch is needed. The store purge happens first
 * and unconditionally, so the device is forgotten (and can't be auto-reconnected) even if the
 * disconnect below does not confirm.
 *
 * @param argc  Argument count from the REPL.
 * @param argv  argv[1] is the selector kind, argv[2] the value.
 * @return      0 if the disconnect was acked, 1 on parse/lookup/transport error.
 */
static int cmd_remove(int argc, char **argv)
{
    uint8_t idx;
    uint8_t mac[TETHER_MAC_LEN];
    if (resolve_target(argc, argv, TARGET_SRC_CONNECTED, &idx, mac) != 0) return 1;

    esp_err_t r = tether_ble_remove(idx, mac, DISCONNECT_TIMEOUT_MS);
    if (r != ESP_OK) {
        printf("removed from database, but C6 disconnect did not confirm: 0x%x\n", r);
        return 1;
    }
    printf("remove request acknowledged (device disconnected and removed from database)\n");
    return 0;
}

/**
 * `disconnect [mac|id|name] <value>` console command. Resolves the target against the connected
 * list and terminates that peer. The C6 acks quickly; the actual disconnect arrives as
 * TETHER_BLE_EVT_PEER_DISCONN in the event callback.
 *
 * @param argc  Argument count from the REPL.
 * @param argv  argv[1] is the selector kind, argv[2] the value.
 * @return      0 on accepted request, 1 on parse/lookup/transport error.
 */
static int cmd_disconnect(int argc, char **argv)
{
    uint8_t idx;
    if (resolve_target(argc, argv, TARGET_SRC_CONNECTED, &idx, NULL) != 0) return 1;

    esp_err_t r = tether_ble_disconnect(idx, DISCONNECT_TIMEOUT_MS);
    if (r != ESP_OK) {
        printf("disconnect failed: 0x%x\n", r);
        return 1;
    }
    printf("disconnect request acknowledged\n");
    return 0;
}

/**
 * `help` console command. Prints a one-line summary for each registered command. Kept in
 * sync with the C6's cmd_help on purpose so both consoles read the same.
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
 * and register every BLE command. Call once during system startup after tether_ble_init.
 */
void tether_ble_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "tether-p4> ";
    repl_config.max_cmdline_length = 64;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
    #error "tether_ble_console_init: no supported console selected in sdkconfig (need UART_DEFAULT, UART_CUSTOM, or USB_SERIAL_JTAG)"
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
    ESP_LOGI(TAG_CON, "P4 console ready: type `help` for commands");
}

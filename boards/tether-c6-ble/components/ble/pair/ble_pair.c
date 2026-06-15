#include "ble_pair.h"
#include "ble_tether.h"
#include "ble_pair_utils.h"
#include "ble_rssi_buff.h"
#include "esp_central.h"
#include "tether_sdio_events.h"

#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tether_sdio_slave.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

static s_adv_t s_adv;
static const char *TAG = "PAIR";


/* ========== Snapshot recording / accessors ========== */

/*
* Records advertisement name, data, and RSSI for a given advertiser address.
*
* @param addr      Advertiser's address (type + 6-byte MAC).
* @param name      Advertiser's name.
* @param name_len  Length of advertiser's name.
* @param data      Advertiser's manufacturer data.
* @param data_len  Length of manufacturer data.
* @param rssi      RSSI value.
*/
void ble_pair_record_adv(const ble_addr_t *addr, const uint8_t *name, uint8_t name_len, const uint8_t *data, uint8_t data_len, int8_t rssi)
{
    if (addr == NULL) {
        return;
    }
    ble_pair_ensure_mtx(&s_adv.mtx);
    if (s_adv.mtx == NULL) {
        return;
    }

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);

    for (uint8_t i = 0; i < s_adv.snapshot.count; i++) {
        if (memcmp(s_adv.snapshot.entries[i], addr->val, 6) == 0) {
            if (s_adv.names[i][0] == '\0' && name != NULL && name_len > 0) {
                copy_adv_name(s_adv.names[i], ADV_NAME_MAX, name, name_len);
            }
            /* Always overwrite mfr data and RSSI — both carry live readings. */
            if (data != NULL && data_len > 0) {
                copy_adv_data(s_adv.data[i], ADV_DATA_MAX, &s_adv.data_len[i], data, data_len);
            }
            s_adv.rssi[i]      = rssi;
            s_adv.last_seen[i] = xTaskGetTickCount();
            xSemaphoreGive(s_adv.mtx);
            return;
        }
    }

    if (s_adv.snapshot.count < MAX_ADVERTISED_DEVICES) {
        uint8_t i = s_adv.snapshot.count;
        memcpy(s_adv.snapshot.entries[i], addr->val, 6);
        s_adv.snapshot.handles[i] = ADV_HANDLE_NONE;
        s_adv.types[i]            = addr->type;
        copy_adv_name(s_adv.names[i], ADV_NAME_MAX, name, name_len);
        copy_adv_data(s_adv.data[i], ADV_DATA_MAX, &s_adv.data_len[i], data, data_len);
        s_adv.rssi[i]      = rssi;
        s_adv.last_seen[i] = xTaskGetTickCount();
        s_adv.snapshot.count++;
    }

    xSemaphoreGive(s_adv.mtx);
}

int ble_show_adv_devices(adv_snapshot_t *out)
{
    if (out == NULL) {
        return 1;
    }
    if (s_adv.mtx == NULL) {
        memset(out, 0, sizeof(*out));
        return 1;
    }

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    sort_snapshot_by_priority(&s_adv);
    *out = s_adv.snapshot;
    xSemaphoreGive(s_adv.mtx);
    return 0;
}

void ble_pair_reset_snapshot(void)
{
    ble_pair_clear_adv(&s_adv);
}

void ble_pair_purge_stale(uint32_t max_age_ms)
{
    if (s_adv.mtx == NULL) return;
    TickType_t threshold = pdMS_TO_TICKS(max_age_ms);
    TickType_t now       = xTaskGetTickCount();

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    uint8_t write = 0;
    for (uint8_t read = 0; read < s_adv.snapshot.count; read++) {
        if ((now - s_adv.last_seen[read]) > threshold) continue;
        if (write != read) swap_adv_slot(&s_adv, write, read);
        write++;
    }
    s_adv.snapshot.count = write;
    xSemaphoreGive(s_adv.mtx);
}


/* ========== Tether Tag filtered view ========== */

// static bool is_tether_tag_name(const char *name)
// {
//     static const char prefix[] = "Tether Tag ";
//     if (strncmp(name, prefix, sizeof(prefix) - 1) != 0) return false;
//     const char *p = name + sizeof(prefix) - 1;
//     if (*p == '\0') return false;
//     for (; *p; p++) {
//         if (*p < '0' || *p > '9') return false;
//     }
//     return true;
// }

int ble_get_tether_tags(scan_report_t *out)
{
    int n;

    if (out == NULL) return 1;
    out->count = 0;
    if (s_adv.mtx == NULL) return 1;

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    for (uint8_t i = 0; i < s_adv.snapshot.count && out->count < MAX_CONNECTED_DEVICES; i++) {
        if (strncmp(s_adv.names[i], "Tether Tag", 10) != 0) continue;
        memcpy(out->entries[out->count], s_adv.snapshot.entries[i], 6);
        out->count++;
    }
    xSemaphoreGive(s_adv.mtx);
    return 0;
}



/* ========== Per-device accessors ========== */

int ble_pair_get_name(uint8_t idx, char *out, uint8_t cap)
{
    if (out == NULL || cap == 0) return 1;
    if (s_adv.mtx == NULL) { out[0] = '\0'; return 1; }

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    if (idx >= s_adv.snapshot.count) {
        xSemaphoreGive(s_adv.mtx);
        out[0] = '\0';
        return 1;
    }
    /* names[] is NUL-terminated at copy_adv_name time. */
    strncpy(out, s_adv.names[idx], cap - 1);
    out[cap - 1] = '\0';
    xSemaphoreGive(s_adv.mtx);
    return 0;
}

int ble_pair_get_rssi(uint8_t idx, int8_t *out)
{
    if (out == NULL) return 1;
    if (s_adv.mtx == NULL) return 1;

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    if (idx >= s_adv.snapshot.count) {
        xSemaphoreGive(s_adv.mtx);
        return 1;
    }
    *out = s_adv.rssi[idx];
    xSemaphoreGive(s_adv.mtx);
    return 0;
}

int ble_pair_get_mfr(uint8_t idx, uint8_t *out, uint8_t cap, uint8_t *out_len)
{
    if (out == NULL || out_len == 0) return 1;
    if (s_adv.mtx == NULL) { *out_len = 0; return 1; }

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    if (idx >= s_adv.snapshot.count) {
        xSemaphoreGive(s_adv.mtx);
        *out_len = 0;
        return 1;
    }
    uint8_t n = s_adv.data_len[idx];
    if (n > cap) n = cap;
    memcpy(out, s_adv.data[idx], n);
    *out_len = n;
    xSemaphoreGive(s_adv.mtx);
    return 0;
}

void ble_pair_clear_mfr(uint8_t idx)
{
    if (s_adv.mtx == NULL) return;
    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    if (idx < s_adv.snapshot.count) {
        s_adv.data_len[idx] = 0;
    }
    xSemaphoreGive(s_adv.mtx);
}

void ble_pair_find_name_by_mac(const uint8_t *mac, char *out, uint8_t cap)
{
    if (out == NULL || cap == 0) return;
    if (mac == NULL || s_adv.mtx == NULL) {
        strncpy(out, "(unknown)", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    const char *n = find_peer_name_by_addr(mac, &s_adv);
    strncpy(out, n, cap - 1);
    out[cap - 1] = '\0';
    xSemaphoreGive(s_adv.mtx);
}

int ble_pair_find_snapshot_index_by_mac(const uint8_t mac[6], uint8_t *out_idx)
{
    if (mac == NULL || out_idx == NULL || s_adv.mtx == NULL) return 1;
    int rc = 1;
    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    for (uint8_t i = 0; i < s_adv.snapshot.count; i++) {
        if (memcmp(s_adv.snapshot.entries[i], mac, 6) == 0) {
            *out_idx = i;
            rc = 0;
            break;
        }
    }
    xSemaphoreGive(s_adv.mtx);
    return rc;
}

int ble_pair_find_snapshot_index_by_name(const char *name, uint8_t *out_idx)
{
    if (name == NULL || out_idx == NULL || s_adv.mtx == NULL) return 1;
    uint8_t hits = 0;
    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    for (uint8_t i = 0; i < s_adv.snapshot.count; i++) {
        if (s_adv.names[i][0] != '\0' && strcasecmp(s_adv.names[i], name) == 0) {
            if (hits == 0) *out_idx = i;
            hits++;
        }
    }
    xSemaphoreGive(s_adv.mtx);
    if (hits == 0) return 1;
    if (hits > 1)  return 2;
    return 0;
}

struct find_connected_ctx {
    bool           by_name;     /* false → match MAC, true → match name */
    const uint8_t *mac;
    const char    *name_ci;
    uint8_t        cursor;
    uint8_t        out_idx;
    uint8_t        hits;
};

static int find_connected_cb(const struct peer *peer, void *arg)
{
    struct find_connected_ctx *ctx = (struct find_connected_ctx *)arg;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(peer->conn_handle, &desc) != 0) return 0;
    const uint8_t *addr = desc.peer_id_addr.val;
    bool match;
    if (ctx->by_name) {
        /* Caller holds s_adv.mtx so this unlocked accessor is safe. */
        const char *n = find_peer_name_by_addr(addr, &s_adv);
        match = (n[0] != '\0' && strcasecmp(n, ctx->name_ci) == 0
                 && strcmp(n, "(unknown)") != 0);
    } else {
        match = (memcmp(addr, ctx->mac, 6) == 0);
    }
    if (match) {
        if (ctx->hits == 0) ctx->out_idx = ctx->cursor;
        ctx->hits++;
    }
    ctx->cursor++;
    return 0;
}

int ble_pair_find_connected_index_by_mac(const uint8_t mac[6], uint8_t *out_idx)
{
    if (mac == NULL || out_idx == NULL) return 1;
    struct find_connected_ctx ctx = { .by_name = false, .mac = mac };
    peer_traverse_all(find_connected_cb, &ctx);
    if (ctx.hits == 0) return 1;
    *out_idx = ctx.out_idx;
    return 0;
}

int ble_pair_find_connected_index_by_name(const char *name, uint8_t *out_idx)
{
    if (name == NULL || out_idx == NULL || s_adv.mtx == NULL) return 1;
    struct find_connected_ctx ctx = { .by_name = true, .name_ci = name };
    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    peer_traverse_all(find_connected_cb, &ctx);
    xSemaphoreGive(s_adv.mtx);
    if (ctx.hits == 0) return 1;
    if (ctx.hits > 1)  return 2;
    *out_idx = ctx.out_idx;
    return 0;
}


/* ========== BLE actions ========== */

/**
 * Returns true if the device name at `idx` starts with "Tether Tag".
 * Must be called with s_adv.mtx held.
 */
static bool is_accel_tag_index(uint8_t idx)
{
    return (strncmp(s_adv.names[idx], "Tether Tag", 10) == 0);
}

int ble_pair_device(uint8_t index)
{
    if (s_adv.mtx == NULL) {
        ESP_LOGW(TAG, "No snapshot yet; run `show` first");
        return 1;
    }

    ble_addr_t peer;

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    if (index >= s_adv.snapshot.count) {
        ESP_LOGW(TAG, "No device at index %u (have %u)", index, s_adv.snapshot.count);
        xSemaphoreGive(s_adv.mtx);
        return 1;
    }

    ESP_LOGI(TAG, "pair_device idx=%u name='%s'", index, s_adv.names[index]);

    peer.type = s_adv.types[index];
    memcpy(peer.val, s_adv.snapshot.entries[index], 6);
    xSemaphoreGive(s_adv.mtx);

    return ble_tether_connect_addr(&peer);
}

int ble_pair_device_mac(const uint8_t *mac)
{
    if (mac == NULL) return 1;
    if (s_adv.mtx == NULL) {
        ESP_LOGW(TAG, "No snapshot yet; run `show` first");
        return 1;
    }

    ble_addr_t peer;
    bool found = false;

    xSemaphoreTake(s_adv.mtx, portMAX_DELAY);
    for (uint8_t i = 0; i < s_adv.snapshot.count; i++) {
        if (memcmp(s_adv.snapshot.entries[i], mac, 6) == 0) {
            peer.type = s_adv.types[i];
            memcpy(peer.val, mac, 6);
            found = true;
            ESP_LOGI(TAG, "pair_device_mac name='%s'", s_adv.names[i]);
            break;
        }
    }
    xSemaphoreGive(s_adv.mtx);

    if (!found) {
        ESP_LOGW(TAG, "MAC not found in snapshot");
        return 1;
    }

    return ble_tether_connect_addr(&peer);
}

struct disconnect_match_ctx {
    uint8_t target_index;
    uint8_t cursor;
    int     terminate_rc;
    bool    found;
};

static int disconnect_match_cb(const struct peer *peer, void *arg)
{
    struct disconnect_match_ctx *ctx = (struct disconnect_match_ctx *)arg;
    if (ctx->found) {
        return 0;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(peer->conn_handle, &desc) != 0) {
        return 0;
    }
    if (ctx->cursor != ctx->target_index) {
        ctx->cursor++;
        return 0;
    }
    ctx->found        = true;
    /* Mark this teardown as command-initiated so BLE_GAP_EVENT_DISCONNECT reports
     * intentional=1 to the P4. peer is const here; fetch the mutable record. */
    struct peer *mut = peer_find(peer->conn_handle);
    if (mut != NULL) {
        mut->intentional_disconnect = true;
    }
    ctx->terminate_rc = ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

struct unpair_match_ctx {
    uint8_t    target_index;
    uint8_t    cursor;
    bool       found;
    ble_addr_t id_addr;    /* resolved identity of the matched peer */
};

static int unpair_match_cb(const struct peer *peer, void *arg)
{
    struct unpair_match_ctx *ctx = (struct unpair_match_ctx *)arg;
    if (ctx->found) {
        return 0;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(peer->conn_handle, &desc) != 0) {
        return 0;
    }
    if (ctx->cursor != ctx->target_index) {
        ctx->cursor++;
        return 0;
    }
    ctx->found   = true;
    /* Capture the resolved identity address — that's what the bond store and the
     * controller resolving list are keyed by, and what ble_gap_unpair() needs. */
    ctx->id_addr = desc.peer_id_addr;

    /* Mark this teardown as command-initiated so BLE_GAP_EVENT_DISCONNECT reports
     * intentional=1 to the P4. peer is const here; fetch the mutable record. */
    struct peer *mut = peer_find(peer->conn_handle);
    if (mut != NULL) {
        mut->intentional_disconnect = true;
    }
    return 0;
}

/**
 * Erase the bond for the Nth currently-connected peer (same ordering as OP_CONNECTED).
 * ble_gap_unpair() deletes the peer's LTK/IRK/CSRK from NVS, removes its identity from
 * the controller resolving list, and terminates the link if it is still up. On success
 * the peer's identity address is written to *out_id (NULL to ignore).
 *
 * @return 0 on success, 1 if no peer at that index, 2 if ble_gap_unpair failed.
 */
static int unpair_nth_connected(uint8_t index, ble_addr_t *out_id)
{
    struct unpair_match_ctx ctx = {
        .target_index = index,
        .cursor       = 0,
        .found        = false,
    };
    peer_traverse_all(unpair_match_cb, &ctx);

    if (!ctx.found) {
        ESP_LOGW(TAG, "No connected device at index %u (run `connected` to list)", index);
        return 1;
    }

    int rc = ble_gap_unpair(&ctx.id_addr);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_unpair failed rc=%d", rc);
        return 2;
    }
    if (out_id != NULL) {
        *out_id = ctx.id_addr;
    }
    return 0;
}

int ble_remove_device(uint8_t index)
{
    /* Console-initiated remove: erase the bond, then tell the P4 to purge the
     * device from its own store. */
    ble_addr_t id;
    if (unpair_nth_connected(index, &id) != 0) {
        return 1;
    }

    tether_event_t evt = { .kind = TETHER_EVT_KIND_REMOVE };
    memcpy(evt.u.remove.mac, id.val, sizeof evt.u.remove.mac);
    tether_sdio_post_event(&evt);

    return 0;
}

int ble_unpair_device(uint8_t index)
{
    /* Host-initiated remove (SDIO OP_REMOVE): erase the bond. The P4 already holds
     * the MAC and purges its own store on the ACK, so no PAIR_REMOVE event is sent. */
    return unpair_nth_connected(index, NULL) == 0 ? 0 : 1;
}

int ble_disconnect_device(uint8_t index)
{
    struct disconnect_match_ctx ctx = {
        .target_index = index,
        .cursor       = 0,
        .terminate_rc = 0,
        .found        = false,
    };
    peer_traverse_all(disconnect_match_cb, &ctx);

    if (!ctx.found) {
        ESP_LOGW(TAG, "No connected device at index %u (run `connected` to list)", index);
        return 1;
    }
    if (ctx.terminate_rc != 0) {
        ESP_LOGW(TAG, "ble_gap_terminate failed rc=%d", ctx.terminate_rc);
        return 1;
    }
    return 0;
}

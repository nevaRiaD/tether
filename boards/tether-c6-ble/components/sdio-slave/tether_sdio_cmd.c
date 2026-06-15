/*
 * Architecture:
 *
 * This file is the command-side of the P4 <-> C6 SDIO transport. The SDIO slave task in
 * tether_sdio_slave.c is responsible for collecting one logical inbound packet (potentially
 * spanning several 128-byte SDIO buffers) into a contiguous frame, then handing it here via
 * tether_sdio_handle_frame(). The wire format is the TLV defined in tether_sdio_proto.h:
 * a 5-byte header followed by an opcode-specific payload.
 *
 * The dispatcher validates the header, switches on the opcode, calls the matching ble_pair_*
 * accessor (the same surface the console REPL uses), and serializes the result into one or
 * more outbound TLV frames. Outbound frames are built into a single DMA-attr scratch buffer
 * (s_tx_scratch) and pushed synchronously through tether_sdio_send_frame(); the dispatcher
 * never holds more than one frame in flight at a time.
 *
 * Responses larger than one SDIO buffer are fragmented at the TLV layer: every fragment
 * carries the same opcode, and all but the last set TETHER_FLAG_MORE so P4 knows to keep
 * reading and concatenating payloads. The dispatcher is the only writer of TX traffic for
 * command responses; events are emitted by tether_sdio_events.c through the same send helper.
 *
 * All BLE state access flows through ble_pair_*; this file never touches the snapshot
 * mutex directly. That keeps both transports (console + SDIO) consistent and avoids
 * doubling up the locking discipline.
 */

#include "tether_sdio_cmd.h"
#include "tether_sdio_proto.h"
#include "tether_sdio_slave.h"

#include "ble_pair.h"
#include "ble_pair_utils.h"
#include "ble_rssi_buff.h"
#include "ble_tether.h"
#include "esp_central.h"

#include "esp_attr.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>


/* ========== File-scope state ========== */

static const char *TAG = "tether_cmd";

/* One scratch frame for building TX. Sized to a single SDIO RX buffer so every frame we
 * send maps cleanly to one buffer on the host side. DMA-capable per sdio_slave_send_queue. */
DMA_ATTR static uint8_t s_tx_scratch[TETHER_MAX_FRAME_SIZE];


/* ========== Forward declarations ========== */

/* Serialize and send one TLV frame using the scratch buffer; returns the underlying SDIO err. */
static esp_err_t send_frame(uint8_t op, uint8_t status, uint8_t flags,
                            const uint8_t *payload, uint16_t plen);

/* Short-hand: send a status-only response (header only, no payload) for the given response op. */
static esp_err_t send_status(uint8_t resp_op, uint8_t status);

/* OP_SHOW handler: walk the snapshot, fragment the slim per-device list if it doesn't fit. */
static void handle_show(void);

/* OP_PAIR handler: parse u8 index, call ble_pair_device, ACK with status only. */
static void handle_pair(const uint8_t *payload, uint16_t plen);

/* OP_PAIR_MAC handler: parse u8 mac[6], call ble_pair_device_mac, ACK with status only. */
static void handle_pair_mac(const uint8_t *payload, uint16_t plen);

/* OP_DISCONNECT handler: parse u8 index, call ble_disconnect_device, ACK with status only. */
static void handle_disconnect(const uint8_t *payload, uint16_t plen);

/* OP_CONNECTED handler: enumerate connected peers via peer_traverse_all, send list. */
static void handle_connected(void);

/* OP_READ_ADV handler: parse u8 index, gather name + mfr data for that device, send detail. */
static void handle_read_adv(const uint8_t *payload, uint16_t plen);

/* OP_PAIR handler: parse u8 index, call ble_pair_device, ACK with status only. */
static void handle_pair(const uint8_t *payload, uint16_t plen);

/* OP_REMOVE handler: parse u8 index, unpair that peer (erase bond + drop link), ACK with
 * status only. The host purges its own store; no PAIR_REMOVE event for host-initiated removes. */
static void handle_remove(const uint8_t *payload, uint16_t plen);

/* OP_DISCONNECT handler: parse u8 index, call ble_disconnect_device, ACK with status only. */
static void handle_disconnect(const uint8_t *payload, uint16_t plen);

/* peer_traverse_all callback for OP_CONNECTED: appends one fixed-width entry to a payload. */
static int append_connected_entry(const struct peer *peer, void *arg);

/* OP_DETECT handler: copy the latest proximity snapshot and ship it as tether_scan_report_t. */
static void handle_detect(void);


/* ========== Function definitions ========== */

/**
 * Build a TLV frame in s_tx_scratch from the given header fields and payload bytes, then
 * hand it to tether_sdio_send_frame for transmission. The caller must keep `plen` <=
 * TETHER_MAX_PAYLOAD_SIZE; the dispatcher enforces this when fragmenting.
 *
 * @param op       Opcode (response or event) to put in the header.
 * @param status   Status byte (TETHER_ST_*). Only meaningful on the final fragment.
 * @param flags    Frame flags (TETHER_FLAG_MORE on non-final fragments, 0 otherwise).
 * @param payload  Pointer to payload bytes to append after the header; may be NULL if plen == 0.
 * @param plen     Number of payload bytes to copy.
 * @return         ESP_OK on success, or the underlying SDIO slave error.
 */
static esp_err_t send_frame(uint8_t op, uint8_t status, uint8_t flags,
                            const uint8_t *payload, uint16_t plen)
{
    if (plen > TETHER_MAX_PAYLOAD_SIZE) {
        ESP_LOGE(TAG, "frame payload %u exceeds max %u", plen, (unsigned)TETHER_MAX_PAYLOAD_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    tether_frame_hdr_t hdr = {
        .op     = op,
        .status = status,
        .flags  = flags,
        .len    = plen,
    };
    memcpy(s_tx_scratch, &hdr, sizeof hdr);
    if (payload != NULL && plen > 0) {
        memcpy(s_tx_scratch + sizeof hdr, payload, plen);
    }
    return tether_sdio_send_frame(s_tx_scratch, sizeof hdr + plen);
}

/**
 * Convenience wrapper that sends an empty-payload response with the given opcode and status.
 *
 * @param resp_op  Response opcode (typically TETHER_OP_*_RESP).
 * @param status   Status byte to put in the header (TETHER_ST_*).
 * @return         ESP_OK on success.
 */
static esp_err_t send_status(uint8_t resp_op, uint8_t status)
{
    return send_frame(resp_op, status, 0, NULL, 0);
}

/**
 * OP_SHOW handler. Copies the current sorted snapshot, walks each entry, and serializes one
 * slim record per device into the response payload. Records are: u8 idx, u8 mac[6], i8 rssi,
 * u8 name_len, char name[name_len] (name truncated to TETHER_SHOW_NAME_MAX). Fragments at
 * frame boundaries; only the last fragment clears TETHER_FLAG_MORE.
 */
static void handle_show(void)
{
    /* When connections are already active, skip the aggressive scan — it competes for radio
     * time and can drop existing connections. The low-duty background scan started at sync
     * and re-asserted after every connect/disconnect keeps the snapshot fresh, so reading it
     * here returns live devices. Only run the full aggressive scan when no tags are connected
     * (i.e. initial pairing from a clean state). */
    if (ble_tether_conn_count() == 0) {
        ble_pair_reset_snapshot();
        ble_tether_start_scan();
        vTaskDelay(pdMS_TO_TICKS(ADV_SCAN_WINDOW_MS));
        ble_tether_stop_scan();
    }

    adv_snapshot_t snap;
    if (ble_show_adv_devices(&snap) != 0) {
        send_status(TETHER_OP_SHOW_RESP, TETHER_ST_NO_SNAPSHOT);
        return;
    }

    uint8_t  buf_payload[TETHER_MAX_PAYLOAD_SIZE];
    uint16_t off = 0;

    for (uint8_t i = 0; i < snap.count; i++) {
        char    name[TETHER_SHOW_NAME_MAX + 1];
        int8_t  rssi = 0;
        ble_pair_get_name(i, name, sizeof name);
        ble_pair_get_rssi(i, &rssi);

        uint8_t name_len = (uint8_t)strlen(name);
        if (name_len > TETHER_SHOW_NAME_MAX) {
            name_len = TETHER_SHOW_NAME_MAX;
        }

        const uint16_t entry_len = 1 + TETHER_MAC_LEN + 1 + 1 + name_len;

        /* If the next entry won't fit in this fragment, flush with MORE=1 and start fresh. */
        if (off + entry_len > TETHER_MAX_PAYLOAD_SIZE) {
            send_frame(TETHER_OP_SHOW_RESP, TETHER_ST_OK, TETHER_FLAG_MORE, buf_payload, off);
            off = 0;
        }

        buf_payload[off++] = i;
        memcpy(&buf_payload[off], snap.entries[i], TETHER_MAC_LEN);
        off += TETHER_MAC_LEN;
        buf_payload[off++] = (uint8_t)rssi;
        buf_payload[off++] = name_len;
        memcpy(&buf_payload[off], name, name_len);
        off += name_len;
    }

    /* Final fragment (or the only fragment, including the empty snapshot case). */
    send_frame(TETHER_OP_SHOW_RESP, TETHER_ST_OK, 0, buf_payload, off);
}

/**
 * peer_traverse_all callback used by handle_connected. Resolves the peer's connection
 * descriptor, live RSSI, and cached display name, then appends one variable-width record
 * {u8 mac[6], i8 rssi, u8 name_len, char name[name_len]} to the payload buffer pointed to
 * by `arg`. Skips peers whose conn_handle is no longer valid.
 *
 * @param peer  NimBLE peer record being visited.
 * @param arg   struct { uint8_t *buf; uint16_t off; uint8_t count; }* — caller's accumulator.
 * @return      Always 0 so peer_traverse_all visits the next peer.
 */
struct conn_accum {
    uint8_t  *buf;
    uint16_t  off;
    uint8_t   count;
};

static int append_connected_entry(const struct peer *peer, void *arg)
{
    struct conn_accum *acc = (struct conn_accum *)arg;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(peer->conn_handle, &desc) != 0) {
        return 0;
    }
    int8_t rssi = 0;
    ble_gap_conn_rssi(peer->conn_handle, &rssi);

    char name[TETHER_CONN_NAME_MAX + 1];
    ble_pair_find_name_by_mac(desc.peer_id_addr.val, name, sizeof name);
    uint8_t name_len = (uint8_t)strlen(name);
    if (name_len > TETHER_CONN_NAME_MAX) {
        name_len = TETHER_CONN_NAME_MAX;
    }

    const uint16_t entry_len = TETHER_MAC_LEN + 1 + 1 + name_len;
    if (acc->off + entry_len > TETHER_MAX_PAYLOAD_SIZE - 1) {
        /* Caller's payload budget exceeded; stop appending but keep iterating. */
        return 0;
    }
    memcpy(&acc->buf[acc->off], desc.peer_id_addr.val, TETHER_MAC_LEN);
    acc->off += TETHER_MAC_LEN;
    acc->buf[acc->off++] = (uint8_t)rssi;
    acc->buf[acc->off++] = name_len;
    memcpy(&acc->buf[acc->off], name, name_len);
    acc->off += name_len;
    acc->count++;
    return 0;
}

static void handle_pair_mac(const uint8_t *payload, uint16_t plen)
{
    if (plen != TETHER_MAC_LEN) {
        send_status(TETHER_OP_PAIR_MAC_RESP, TETHER_ST_BAD_LEN);
        return;
    }
    int rc = ble_pair_device_mac(payload);
    send_status(TETHER_OP_PAIR_MAC_RESP, rc == 0 ? TETHER_ST_OK : TETHER_ST_BLE_ERR);
}

/**
 * OP_CONNECTED handler. Walks every currently connected peer via peer_traverse_all and
 * builds a response of [u8 count, {u8 mac[6], i8 rssi, u8 name_len, char name[name_len]} x count].
 * Fits in a single frame given the connection cap is well under what TETHER_MAX_PAYLOAD_SIZE can hold.
 */
static void handle_connected(void)
{
    uint8_t buf_payload[TETHER_MAX_PAYLOAD_SIZE];
    struct conn_accum acc = { .buf = &buf_payload[1], .off = 0, .count = 0 };
    peer_traverse_all(append_connected_entry, &acc);
    buf_payload[0] = acc.count;
    send_frame(TETHER_OP_CONNECTED_RESP, TETHER_ST_OK, 0, buf_payload, 1 + acc.off);
}

/**
 * OP_READ_ADV handler. Expects a single byte payload (`index`). Reads the cached name and
 * manufacturer data for that index from the snapshot and serializes them into one response
 * frame as: u8 idx, u8 mac[6], u8 name_len, char name[name_len], u8 data_len, u8 data[data_len].
 * No live rescan is performed here (unlike the console `read_adv` command) because the SDIO
 * dispatcher should not block on a multi-second BLE scan; P4 can call OP_SHOW first to force
 * a fresh window.
 *
 * @param payload  Pointer to the inbound payload bytes.
 * @param plen     Length of the payload (must be exactly 1).
 */
static void handle_read_adv(const uint8_t *payload, uint16_t plen)
{
    if (plen != 1) {
        send_status(TETHER_OP_READ_ADV_RESP, TETHER_ST_BAD_LEN);
        return;
    }
    uint8_t idx = payload[0];

    adv_snapshot_t snap;
    if (ble_show_adv_devices(&snap) != 0) {
        send_status(TETHER_OP_READ_ADV_RESP, TETHER_ST_NO_SNAPSHOT);
        return;
    }
    if (idx >= snap.count) {
        send_status(TETHER_OP_READ_ADV_RESP, TETHER_ST_BAD_INDEX);
        return;
    }

    char    name[ADV_NAME_MAX];
    uint8_t data[ADV_DATA_MAX];
    uint8_t dlen = 0;
    ble_pair_get_name(idx, name, sizeof name);
    ble_pair_get_mfr(idx, data, sizeof data, &dlen);

    uint8_t name_len = (uint8_t)strlen(name);

    uint8_t buf_payload[TETHER_MAX_PAYLOAD_SIZE];
    uint16_t off = 0;
    buf_payload[off++] = idx;
    memcpy(&buf_payload[off], snap.entries[idx], TETHER_MAC_LEN);
    off += TETHER_MAC_LEN;
    buf_payload[off++] = name_len;
    memcpy(&buf_payload[off], name, name_len);
    off += name_len;
    buf_payload[off++] = dlen;
    memcpy(&buf_payload[off], data, dlen);
    off += dlen;

    send_frame(TETHER_OP_READ_ADV_RESP, TETHER_ST_OK, 0, buf_payload, off);
}

/**
 * OP_PAIR handler. Expects a single byte payload (`index` into the snapshot). Invokes
 * ble_pair_device(); the actual BLE connection completes asynchronously and EVT_PAIR_COMPLETE
 * will follow. The response here is just an ACK indicating the request was accepted (or
 * rejected at the input layer).
 *
 * @param payload  Pointer to the inbound payload bytes.
 * @param plen     Length of the payload (must be exactly 1).
 */
static void handle_pair(const uint8_t *payload, uint16_t plen)
{
    if (plen != 1) {
        send_status(TETHER_OP_PAIR_RESP, TETHER_ST_BAD_LEN);
        return;
    }
    uint8_t idx = payload[0];
    int rc = ble_pair_device(idx);
    send_status(TETHER_OP_PAIR_RESP, rc == 0 ? TETHER_ST_OK : TETHER_ST_BLE_ERR);
}

/**
 * OP_DISCONNECT handler. Expects a single byte payload (`index`). Invokes ble_disconnect_device
 * which terminates the Nth currently-connected peer (same ordering as OP_CONNECTED). The host
 * should index against the most recent OP_CONNECTED response, not OP_SHOW — iOS/LE-Privacy
 * peers have an identity address that differs from the address they advertised with. The
 * EVT_PEER_DISCONN event fires from the GAP callback once the disconnect lands.
 *
 * @param payload  Pointer to the inbound payload bytes.
 * @param plen     Length of the payload (must be exactly 1).
 */
static void handle_disconnect(const uint8_t *payload, uint16_t plen)
{
    if (plen != 1) {
        send_status(TETHER_OP_DISCONNECT_RESP, TETHER_ST_BAD_LEN);
        return;
    }
    int rc = ble_disconnect_device(payload[0]);
    send_status(TETHER_OP_DISCONNECT_RESP, rc == 0 ? TETHER_ST_OK : TETHER_ST_BLE_ERR);
}

/**
 * OP_REMOVE handler. Expects a single byte payload (`index`). Unpairs the Nth currently-
 * connected peer (same ordering as OP_CONNECTED): unlike OP_DISCONNECT, this erases the
 * peer's bond — deleting its LTK/IRK/CSRK from NVS and dropping it from the controller
 * resolving list — in addition to terminating the link. The host initiated the remove and
 * already holds the device's MAC, so it purges the device from its own store after this ACK;
 * the C6 does not echo a PAIR_REMOVE event for host-initiated removes (that event path is
 * for removes started on the C6 console).
 *
 * @param payload  Pointer to the inbound payload bytes.
 * @param plen     Length of the payload (must be exactly 1).
 */
static void handle_remove(const uint8_t *payload, uint16_t plen)
{
    if (plen != 1) {
        send_status(TETHER_OP_REMOVE_RESP, TETHER_ST_BAD_LEN);
        return;
    }
    int rc = ble_unpair_device(payload[0]);
    send_status(TETHER_OP_REMOVE_RESP, rc == 0 ? TETHER_ST_OK : TETHER_ST_BLE_ERR);
}

/**
 * OP_DETECT handler. Copies the most recent close-and-moving proximity snapshot maintained by
 * the BLE polling task and ships it as the OP_DETECT_RESP response. The snapshot is pulled on
 * demand — no live BLE scan runs here — so this handler returns promptly. The struct is
 * memcpy'd straight onto the wire (layout pinned by tether_scan_report_t in the proto header)
 * and fragmented across TLV frames with TETHER_FLAG_MORE when it exceeds one SDIO buffer;
 * only the last fragment clears the flag. An empty/zeroed snapshot still sends one frame.
 */
static void handle_detect(void)
{
    _Static_assert(sizeof(scan_report_t) == sizeof(tether_scan_report_t),
                   "C6 scan_report_t must match wire tether_scan_report_t");

    scan_report_t report;
    ble_get_latest_scan(&report);  /* zeroes report (count == 0) if the polling task has not run */

    const uint8_t *src   = (const uint8_t *)&report;
    const size_t   total = sizeof report;
    size_t         off   = 0;

    do {
        size_t  chunk = total - off;
        uint8_t flags = 0;
        if (chunk > TETHER_MAX_PAYLOAD_SIZE) {
            chunk = TETHER_MAX_PAYLOAD_SIZE;
            flags = TETHER_FLAG_MORE;
        }
        send_frame(TETHER_OP_DETECT_RESP, TETHER_ST_OK, flags, src + off, (uint16_t)chunk);
        off += chunk;
    } while (off < total);
}

/**
 * Public entry point. Parses the TLV header on a fully-assembled inbound frame and routes
 * to the per-opcode handler. Rejects frames that are too short, that have a payload length
 * inconsistent with the buffer size, that carry the MORE flag (multi-fragment commands
 * are not supported yet), or whose opcode falls outside the command range.
 *
 * @param buf  Pointer to the start of the assembled frame (header first).
 * @param len  Total bytes in the frame (header + payload).
 */
void tether_sdio_handle_frame(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < TETHER_FRAME_HDR_SIZE) {
        ESP_LOGW(TAG, "frame too short (%u bytes)", (unsigned)len);
        return;
    }
    tether_frame_hdr_t hdr;
    memcpy(&hdr, buf, sizeof hdr);

    if ((size_t)hdr.len + TETHER_FRAME_HDR_SIZE > len) {
        ESP_LOGW(TAG, "frame len=%u exceeds buffer size %u", hdr.len, (unsigned)len);
        send_status(hdr.op | TETHER_OP_RESP_MASK, TETHER_ST_BAD_LEN);
        return;
    }
    if (hdr.flags & TETHER_FLAG_MORE) {
        ESP_LOGW(TAG, "multi-fragment commands not supported");
        send_status(hdr.op | TETHER_OP_RESP_MASK, TETHER_ST_BAD_LEN);
        return;
    }
    if (hdr.op & TETHER_OP_RESP_MASK) {
        ESP_LOGW(TAG, "ignoring opcode 0x%02x (not a command)", hdr.op);
        return;
    }

    const uint8_t *payload = buf + TETHER_FRAME_HDR_SIZE;
    const uint16_t plen    = hdr.len;

    switch (hdr.op) {
    case TETHER_OP_SHOW:
        handle_show();
        break;
    case TETHER_OP_CONNECTED:
        handle_connected();
        break;
    case TETHER_OP_PAIR_MAC:
        handle_pair_mac(payload, plen);
        break;
    case TETHER_OP_READ_ADV:
        handle_read_adv(payload, plen);
        break;
    case TETHER_OP_PAIR:
        handle_pair(payload, plen);
        break;
    case TETHER_OP_REMOVE:
        handle_remove(payload, plen);
        break;
    case TETHER_OP_DISCONNECT:
        handle_disconnect(payload, plen);
        break;
    case TETHER_OP_DETECT:
        handle_detect();
        break;
    default:
        ESP_LOGW(TAG, "unknown opcode 0x%02x", hdr.op);
        send_status(hdr.op | TETHER_OP_RESP_MASK, TETHER_ST_BAD_OPCODE);
        break;
    }
}

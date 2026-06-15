/*
 * P4-side BLE command channel over SDIO.
 *
 * Sits on top of sdio-master (essl_*) and presents the rest of the P4 app with
 * a synchronous command API plus an async event callback. The wire format is
 * defined in tether_sdio_proto.h (mirrored on the C6 side).
 *
 * Concurrency model:
 *   - One internal "tether-ble" task drains SDIO packets, parses TLV frames,
 *     reassembles FLAG_MORE-fragmented messages, and dispatches to either the
 *     waiting requester or the registered event callback.
 *   - Public command functions block the calling task on a binary semaphore
 *     given by the task when the matching response lands. An internal mutex
 *     serializes concurrent callers so only one command is ever in flight.
 *   - The response payload is reassembled into a 4 KB scratch slot owned by
 *     the tether-ble task. It is valid from the moment the semaphore is given
 *     until the calling task releases the request mutex, after which the slot
 *     may be reused by a later command.
 *   - Events have their own reassembly slot (separate from the response slot) so a
 *     partially-reassembled event can coexist with an in-flight command response.
 *     The callback fires once per logical event on the tether-ble task; keep work brief.
 *
 * The proximity snapshot is pulled on demand: tether_ble_detected() issues an OP_DETECT
 * command and parses the OP_DETECT_RESP response. Events carry only PAIR_COMPLETE and
 * PEER_DISCONN — things the C6 raises on its own timeline.
 *
 * Failure handling:
 *   - sdio_master_send_packet errors propagate to the caller; the in-flight
 *     state is cleared before returning.
 *   - Response timeouts clear s_expected_op so a late response is dropped.
 *   - Status codes from the C6 are mapped to esp_err_t via status_to_err.
 */

#include "tether_ble.h"
#include "sdio_master.h"
#include "tether_sdio_proto.h"
#include "tether_config.h"
#include "store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"


/* ========== Config / state ========== */

/* Sized for the worst-case OP_SHOW response: 255 devices x TETHER_SHOW_ENTRY_MAX (~29 B). */
#define TETHER_BLE_RESP_SLOT_SIZE   8192
#define TETHER_BLE_EVT_SLOT_SIZE    512
#define TETHER_BLE_SEND_TIMEOUT_MS  1000

#define TAG_BLE                     "tether_ble"

#define TETHER_BLE_CHECK(cond, err_val) do {                                 \
    if (!(cond)) {                                                           \
        ESP_LOGE(TAG_BLE, "%s(%d): check failed: %s",                        \
                 __FUNCTION__, __LINE__, #cond);                             \
        return (err_val);                                                    \
    }                                                                        \
} while (0)

static essl_handle_t          s_handle      = NULL;
static SemaphoreHandle_t      s_request_mtx = NULL;
static SemaphoreHandle_t      s_resp_sem    = NULL;
static TaskHandle_t           s_task        = NULL;

static volatile uint8_t       s_expected_op = 0;
static uint8_t                s_resp_status = 0;
static uint8_t                s_resp_slot[TETHER_BLE_RESP_SLOT_SIZE];
static size_t                 s_resp_slot_len = 0;

static tether_ble_event_cb_t  s_event_cb    = NULL;
static void                  *s_event_user  = NULL;

/* Event reassembly state. Events use FLAG_MORE the same way responses do, but live in
 * their own buffer so a partially-reassembled event can coexist with an in-flight response. */
static uint8_t                s_evt_buf[TETHER_BLE_EVT_SLOT_SIZE];
static size_t                 s_evt_buf_len = 0;
static uint8_t                s_evt_in_progress_op = 0;

DMA_ATTR static uint8_t       s_rx_frame[TETHER_MAX_FRAME_SIZE];


/* ========== Forward declarations ========== */

/* FreeRTOS task entry: drain SDIO packets, parse TLV, route to response slot or event callback. */
static void tether_ble_task(void *arg);

/* Process one TLV frame whose opcode is in the response range (0x80–0xBF).            */
static void handle_response_frame(const tether_frame_hdr_t *hdr, const uint8_t *payload);

/* Process one TLV frame whose opcode is in the event range (0xC0–0xFF).               */
static void handle_event_frame(const tether_frame_hdr_t *hdr, const uint8_t *payload);

/* Build one TLV command frame on the stack and hand it to sdio_master_send_packet.    */
static esp_err_t send_command(uint8_t op, const uint8_t *payload, uint16_t payload_len);

/* Send a command, then wait on s_resp_sem for the matching response or timeout.       */
static esp_err_t transact(uint8_t cmd_op, const uint8_t *req_payload, uint16_t req_len,
                          uint32_t timeout_ms);

/* Parse OP_SHOW_RESP payload (sequence of variable-length per-device entries).        */
static esp_err_t parse_show(const uint8_t *p, size_t plen,
                            tether_dev_t *out, uint8_t cap, uint8_t *count);

/* Parse OP_CONNECTED_RESP payload (count-prefixed fixed-width entries).               */
static esp_err_t parse_connected(const uint8_t *p, size_t plen,
                                 tether_conn_t *out, uint8_t cap, uint8_t *count);

/* Parse OP_READ_ADV_RESP payload (one device's full detail).                          */
static esp_err_t parse_read_adv(const uint8_t *p, size_t plen, tether_adv_detail_t *out);

/* Parse OP_DETECT_RESP payload (the tether_scan_report_t wire layout).                 */
static esp_err_t parse_detected(const uint8_t *p, size_t plen, tether_scan_report_t *out);

/* Map a TETHER_ST_* code from the slave into an esp_err_t the caller will recognize.  */
static esp_err_t status_to_err(uint8_t status);


/* ========== Function definitions ========== */

/**
 * Task loop. Polls sdio_master_process_event for inbound SDIO packets, validates the TLV
 * header, and dispatches based on opcode class. Frames with FLAG_MORE set are appended to
 * the response slot; a frame without the flag (matching the expected opcode) closes the
 * logical message and signals the waiter.
 *
 * @param arg  Unused.
 */
static void tether_ble_task(void *arg)
{
    (void)arg;

    for (;;) {
        size_t    got = 0;
        /* UINT32_MAX = block forever in essl_wait_int; the task wakes only when the slave
         * actually raises an interrupt on DAT1, instead of polling the bus continuously. */
        esp_err_t r   = sdio_master_process_event(s_handle, s_rx_frame, sizeof s_rx_frame, &got, UINT32_MAX);

        if (r == ESP_ERR_TIMEOUT || got == 0) {
            continue;
        }
        if (r != ESP_OK && r != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG_BLE, "process_event err 0x%x", r);
            continue;
        }
        if (got < TETHER_FRAME_HDR_SIZE) {
            ESP_LOGW(TAG_BLE, "runt frame: %u bytes", (unsigned)got);
            continue;
        }

        tether_frame_hdr_t hdr;
        memcpy(&hdr, s_rx_frame, sizeof hdr);

        if ((size_t)TETHER_FRAME_HDR_SIZE + hdr.len > got) {
            ESP_LOGW(TAG_BLE, "truncated frame: hdr.len=%u got=%u", hdr.len, (unsigned)got);
            continue;
        }
        const uint8_t *payload = s_rx_frame + TETHER_FRAME_HDR_SIZE;

        if ((hdr.op & TETHER_OP_EVT_MASK) == TETHER_OP_EVT_MASK) {
            /* 0xC0..0xFF — event */
            handle_event_frame(&hdr, payload);
        } else if (hdr.op & TETHER_OP_RESP_MASK) {
            /* 0x80..0xBF — response */
            handle_response_frame(&hdr, payload);
        } else {
            ESP_LOGW(TAG_BLE, "unexpected command opcode 0x%02X from slave", hdr.op);
        }
    }
}

/**
 * Append (or set) the payload of one response-class frame into the response slot, and signal
 * the waiter when this is the final frame of a logical message. Stale frames whose opcode does
 * not match s_expected_op are logged and dropped.
 *
 * @param hdr      Parsed frame header.
 * @param payload  Pointer to payload bytes within s_rx_frame.
 */
static void handle_response_frame(const tether_frame_hdr_t *hdr, const uint8_t *payload)
{
    if (s_expected_op == 0 || s_expected_op != hdr->op) {
        ESP_LOGW(TAG_BLE, "unsolicited/mismatched response op=0x%02X expected=0x%02X",
                 hdr->op, s_expected_op);
        return;
    }
    if (s_resp_slot_len + hdr->len > sizeof s_resp_slot) {
        ESP_LOGE(TAG_BLE, "response slot overflow (%u + %u > %u)",
                 (unsigned)s_resp_slot_len, hdr->len, (unsigned)sizeof s_resp_slot);
        s_resp_slot_len = 0;
        return;
    }
    memcpy(s_resp_slot + s_resp_slot_len, payload, hdr->len);
    s_resp_slot_len += hdr->len;

    if (!(hdr->flags & TETHER_FLAG_MORE)) {
        s_resp_status = hdr->status;
        xSemaphoreGive(s_resp_sem);
    }
}

/**
 * Append one event-class frame's payload into the event reassembly buffer; when the final
 * fragment lands (FLAG_MORE cleared), parse the full payload and invoke the registered
 * callback. The event buffer is separate from the response slot so a partially-reassembled
 * event does not collide with an in-flight command response.
 *
 * @param hdr      Parsed frame header.
 * @param payload  Pointer to payload bytes within s_rx_frame.
 */
static void handle_event_frame(const tether_frame_hdr_t *hdr, const uint8_t *payload)
{
    if (s_evt_in_progress_op == 0) {
        s_evt_buf_len        = 0;
        s_evt_in_progress_op = hdr->op;
    } else if (s_evt_in_progress_op != hdr->op) {
        ESP_LOGW(TAG_BLE, "event op switched mid-reassembly: 0x%02X -> 0x%02X",
                 s_evt_in_progress_op, hdr->op);
        s_evt_buf_len        = 0;
        s_evt_in_progress_op = hdr->op;
    }

    if (s_evt_buf_len + hdr->len > sizeof s_evt_buf) {
        ESP_LOGE(TAG_BLE, "event slot overflow (%u + %u > %u)",
                 (unsigned)s_evt_buf_len, hdr->len, (unsigned)sizeof s_evt_buf);
        s_evt_buf_len        = 0;
        s_evt_in_progress_op = 0;
        return;
    }
    memcpy(s_evt_buf + s_evt_buf_len, payload, hdr->len);
    s_evt_buf_len += hdr->len;

    if (hdr->flags & TETHER_FLAG_MORE) {
        return;
    }

    /* Final fragment — snapshot the logical message and reset reassembly state. */
    uint8_t op       = s_evt_in_progress_op;
    size_t  body_len = s_evt_buf_len;
    s_evt_in_progress_op = 0;
    s_evt_buf_len        = 0;

    if (s_event_cb == NULL) {
        return;
    }

    tether_ble_event_t evt = {0};
    Tag *tag;
    switch (op) {
    case TETHER_EVT_PAIR_COMPLETE:
        if (body_len < TETHER_MAC_LEN + 1) {
            ESP_LOGW(TAG_BLE, "PAIR_COMPLETE payload too short: %u", (unsigned)body_len);
            return;
        }
        evt.kind = TETHER_BLE_EVT_PAIR_COMPLETE;
        memcpy(evt.pair_complete.mac, s_evt_buf, TETHER_MAC_LEN);
        evt.pair_complete.success = s_evt_buf[TETHER_MAC_LEN] != 0;
        /* connection_type byte is optional: older C6 firmware omits it → default 0.
         * 1 = central→peripheral (C6 connected to a tag, fires on connect),
         * 2 = peripheral→central (phone bonded to the C6, fires on encryption). */
        evt.pair_complete.connection_type =
            (body_len > TETHER_MAC_LEN + 1)
                ? (tether_conn_type_t)s_evt_buf[TETHER_MAC_LEN + 1]
                : (tether_conn_type_t)0;
        break;
    case TETHER_EVT_PEER_DISCONN:
        if (body_len < TETHER_MAC_LEN + 2) {
            ESP_LOGW(TAG_BLE, "PEER_DISCONN payload too short: %u", (unsigned)body_len);
            return;
        }
        evt.kind = TETHER_BLE_EVT_PEER_DISCONN;
        memcpy(evt.peer_disconn.mac, s_evt_buf, TETHER_MAC_LEN);
        memcpy(&evt.peer_disconn.reason, s_evt_buf + TETHER_MAC_LEN, sizeof(uint16_t));
        /* intentional byte is optional: older C6 firmware omits it → default false. */
        evt.peer_disconn.intentional =
            (body_len > TETHER_MAC_LEN + 2) && (s_evt_buf[TETHER_MAC_LEN + 2] != 0);
        break;
    case TETHER_EVT_PAIR_REMOVE:
        evt.kind = TETHER_BLE_EVT_PAIR_REMOVE;
        memcpy(evt.pair_remove.mac, s_evt_buf, TETHER_MAC_LEN);
        if (store_tag_find_by_mac(evt.pair_remove.mac, &tag) == ESP_OK && tag != NULL) {
            store_tag_delete(tag->tag_id);
        }
        break;
    default:
        ESP_LOGW(TAG_BLE, "unknown event op 0x%02X", op);
        return;
    }
    s_event_cb(&evt, s_event_user);
}

/**
 * Assemble a single TLV command frame in a stack buffer and hand it to sdio_master_send_packet.
 * Caller is responsible for holding s_request_mtx so no other command's payload is in flight.
 *
 * @param op           Opcode (0x01..0x7F).
 * @param payload      Pointer to request payload, or NULL if empty.
 * @param payload_len  Payload length in bytes; must be <= TETHER_MAX_PAYLOAD_SIZE.
 * @return             ESP_OK on successful queue, ESP_ERR_INVALID_ARG if oversize, or an
 *                     error from the SDIO master driver.
 */
static esp_err_t send_command(uint8_t op, const uint8_t *payload, uint16_t payload_len)
{
    TETHER_BLE_CHECK(payload_len <= TETHER_MAX_PAYLOAD_SIZE, ESP_ERR_INVALID_ARG);

    uint8_t frame[TETHER_MAX_FRAME_SIZE];
    tether_frame_hdr_t hdr = { .op = op, .status = 0, .flags = 0, .len = payload_len };
    memcpy(frame, &hdr, sizeof hdr);
    if (payload != NULL && payload_len > 0) {
        memcpy(frame + sizeof hdr, payload, payload_len);
    }
    return sdio_master_send_packet(s_handle, frame, sizeof hdr + payload_len,
                                   TETHER_BLE_SEND_TIMEOUT_MS);
}

/**
 * Issue one command and block until the matching response frame closes the logical message.
 * Caller must already hold s_request_mtx. On return, s_resp_status and s_resp_slot[0..len)
 * are valid for the remainder of the mutex hold.
 *
 * @param cmd_op       Command opcode (0x01..0x7F).
 * @param req_payload  Pointer to request payload, or NULL if empty.
 * @param req_len      Length of the request payload.
 * @param timeout_ms   Maximum time to wait for the response.
 * @return             ESP_OK if a response landed, ESP_ERR_TIMEOUT on response wait timeout,
 *                     or an error from the SDIO send path.
 */
static esp_err_t transact(uint8_t cmd_op, const uint8_t *req_payload, uint16_t req_len,
                          uint32_t timeout_ms)
{
    while (xSemaphoreTake(s_resp_sem, 0) == pdTRUE) {
        /* drain any stale signal left from a prior aborted transaction */
    }
    s_resp_slot_len = 0;
    s_expected_op   = cmd_op | TETHER_OP_RESP_MASK;

    esp_err_t r = send_command(cmd_op, req_payload, req_len);
    if (r != ESP_OK) {
        s_expected_op = 0;
        return r;
    }

    if (xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_expected_op = 0;
        return ESP_ERR_TIMEOUT;
    }
    s_expected_op = 0;
    return ESP_OK;
}

/**
 * Parse OP_SHOW_RESP payload. Each entry is variable-length:
 *   u8 idx, u8 mac[6], i8 rssi, u8 name_len, char name[name_len].
 * Fills up to `cap` slots in `out`; further entries on the wire are skipped.
 *
 * @param p       Payload bytes.
 * @param plen    Payload length.
 * @param out     Caller-provided array of cap entries.
 * @param cap     Maximum entries to fill.
 * @param count   Receives the number filled.
 * @return        ESP_OK on success, ESP_ERR_INVALID_RESPONSE if framing is malformed.
 */
static esp_err_t parse_show(const uint8_t *p, size_t plen,
                            tether_dev_t *out, uint8_t cap, uint8_t *count)
{
    *count = 0;
    size_t off = 0;
    while (off < plen) {
        const size_t hdr_min = 1 + TETHER_MAC_LEN + 1 + 1;
        if (plen - off < hdr_min) return ESP_ERR_INVALID_RESPONSE;

        uint8_t  wire_name_len = p[off + 1 + TETHER_MAC_LEN + 1];
        size_t   entry_size    = hdr_min + wire_name_len;
        if (plen - off < entry_size) return ESP_ERR_INVALID_RESPONSE;

        if (*count < cap) {
            tether_dev_t *d = &out[*count];
            d->idx = p[off];
            memcpy(d->mac, p + off + 1, TETHER_MAC_LEN);
            d->rssi = (int8_t)p[off + 1 + TETHER_MAC_LEN];

            uint8_t copy = wire_name_len > TETHER_SHOW_NAME_MAX
                         ? TETHER_SHOW_NAME_MAX : wire_name_len;
            memcpy(d->name, p + off + hdr_min, copy);
            d->name[copy] = '\0';
            (*count)++;
        }
        off += entry_size;
    }
    return ESP_OK;
}

/**
 * Parse OP_CONNECTED_RESP payload. Leads with a u8 count, then `count` variable-length entries:
 *   u8 mac[6], i8 rssi, u8 name_len, char name[name_len].
 * Fills up to `cap` slots in `out`; further entries on the wire are skipped. Each filled slot's
 * array position matches the wire order, so it is also the index OP_DISCONNECT expects.
 *
 * @param p       Payload bytes.
 * @param plen    Payload length.
 * @param out     Caller-provided array of cap entries.
 * @param cap     Maximum entries to fill.
 * @param count   Receives the number filled (clipped to cap).
 * @return        ESP_OK on success, ESP_ERR_INVALID_RESPONSE if framing is malformed.
 */
static esp_err_t parse_connected(const uint8_t *p, size_t plen,
                                 tether_conn_t *out, uint8_t cap, uint8_t *count)
{
    TETHER_BLE_CHECK(plen >= 1, ESP_ERR_INVALID_RESPONSE);
    uint8_t wire_count = p[0];
    *count = 0;
    size_t off = 1;
    for (uint8_t i = 0; i < wire_count; i++) {
        const size_t hdr_min = TETHER_MAC_LEN + 1 + 1;
        if (plen - off < hdr_min) return ESP_ERR_INVALID_RESPONSE;

        uint8_t wire_name_len = p[off + TETHER_MAC_LEN + 1];
        size_t  entry_size    = hdr_min + wire_name_len;
        if (plen - off < entry_size) return ESP_ERR_INVALID_RESPONSE;

        if (*count < cap) {
            tether_conn_t *c = &out[*count];
            memcpy(c->mac, p + off, TETHER_MAC_LEN);
            c->rssi = (int8_t)p[off + TETHER_MAC_LEN];

            uint8_t copy = wire_name_len > TETHER_CONN_NAME_MAX
                         ? TETHER_CONN_NAME_MAX : wire_name_len;
            memcpy(c->name, p + off + hdr_min, copy);
            c->name[copy] = '\0';
            (*count)++;
        }
        off += entry_size;
    }
    return ESP_OK;
}

/**
 * Parse OP_READ_ADV_RESP payload (single device's full detail). Name is NUL-terminated
 * in the output struct; data is copied up to TETHER_READ_ADV_DATA_MAX bytes.
 *
 * @param p     Payload bytes.
 * @param plen  Payload length.
 * @param out   Caller-provided detail struct.
 * @return      ESP_OK on success, ESP_ERR_INVALID_RESPONSE if framing is malformed.
 */
static esp_err_t parse_read_adv(const uint8_t *p, size_t plen, tether_adv_detail_t *out)
{
    const size_t name_off = 1 + TETHER_MAC_LEN + 1;
    TETHER_BLE_CHECK(plen >= name_off, ESP_ERR_INVALID_RESPONSE);

    uint8_t name_len = p[1 + TETHER_MAC_LEN];
    TETHER_BLE_CHECK(plen >= name_off + name_len + 1, ESP_ERR_INVALID_RESPONSE);

    uint8_t data_len = p[name_off + name_len];
    TETHER_BLE_CHECK(plen >= name_off + name_len + 1 + data_len, ESP_ERR_INVALID_RESPONSE);

    out->idx = p[0];
    memcpy(out->mac, p + 1, TETHER_MAC_LEN);

    uint8_t cn = name_len > TETHER_READ_ADV_NAME_MAX ? TETHER_READ_ADV_NAME_MAX : name_len;
    memcpy(out->name, p + name_off, cn);
    out->name[cn] = '\0';

    uint8_t cd = data_len > TETHER_READ_ADV_DATA_MAX ? TETHER_READ_ADV_DATA_MAX : data_len;
    memcpy(out->data, p + name_off + name_len + 1, cd);
    out->data_len = cd;
    return ESP_OK;
}

/**
 * Parse an OP_DETECT_RESP payload — the tether_scan_report_t wire layout: u8 count, then
 * count MAC entries of TETHER_MAC_LEN bytes each (no rssi; this is the proximity snapshot,
 * not the connected list).
 *
 * @param p     Payload bytes.
 * @param plen  Payload length.
 * @param out   Receives the parsed snapshot (count + MAC entries).
 * @return      ESP_OK on success, ESP_ERR_INVALID_RESPONSE if framing is malformed.
 */
static esp_err_t parse_detected(const uint8_t *p, size_t plen, tether_scan_report_t *out)
{
    TETHER_BLE_CHECK(plen >= 1, ESP_ERR_INVALID_RESPONSE);
    uint8_t wire_count = p[0];
    if (wire_count > TETHER_SCAN_MAX ||
        plen < 1 + (size_t)wire_count * TETHER_MAC_LEN) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out->count = wire_count;
    memcpy(out->entries, p + 1, (size_t)wire_count * TETHER_MAC_LEN);
    return ESP_OK;
}

/**
 * Translate a TETHER_ST_* code from the slave into the most descriptive esp_err_t.
 *
 * @param status  Status byte from a response frame.
 * @return        ESP_OK if status == TETHER_ST_OK, otherwise a matching error.
 */
static esp_err_t status_to_err(uint8_t status)
{
    switch (status) {
    case TETHER_ST_OK:           return ESP_OK;
    case TETHER_ST_BAD_OPCODE:   return ESP_ERR_INVALID_ARG;
    case TETHER_ST_BAD_LEN:      return ESP_ERR_INVALID_SIZE;
    case TETHER_ST_BAD_INDEX:    return ESP_ERR_INVALID_ARG;
    case TETHER_ST_NO_SNAPSHOT:  return ESP_ERR_INVALID_STATE;
    case TETHER_ST_BUSY:         return ESP_ERR_INVALID_STATE;
    case TETHER_ST_BLE_ERR:      return ESP_FAIL;
    case TETHER_ST_INTERNAL:
    default:                     return ESP_FAIL;
    }
}


/* ========== Public API ========== */

/**
 * Bring up the tether-ble layer on an already-initialized ESSL handle. Creates the request
 * mutex, response semaphore, and spawns the receive task at SDIO_MASTER_TASK_PRIO.
 *
 * @param handle  Live ESSL handle from sdio_master_init.
 * @return        ESP_OK on success, ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM on failure.
 */
esp_err_t tether_ble_init(essl_handle_t handle)
{
    TETHER_BLE_CHECK(handle != NULL, ESP_ERR_INVALID_ARG);
    TETHER_BLE_CHECK(s_handle == NULL, ESP_ERR_INVALID_STATE);

    s_request_mtx = xSemaphoreCreateMutex();
    s_resp_sem    = xSemaphoreCreateBinary();
    if (s_request_mtx == NULL || s_resp_sem == NULL) {
        ESP_LOGE(TAG_BLE, "RTOS object creation failed");
        return ESP_ERR_NO_MEM;
    }

    s_handle = handle;
    BaseType_t ok = xTaskCreate(tether_ble_task, "tether_ble",
                                SDIO_MASTER_TASK_STACK, NULL,
                                SDIO_MASTER_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG_BLE, "task create failed");
        s_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG_BLE, "ready");
    return ESP_OK;
}

/**
 * Register (or clear, with NULL) the async event callback. Subsequent EVT_* frames from the
 * C6 will invoke this callback in the tether-ble task context.
 *
 * @param cb    Callback function pointer, or NULL to disable.
 * @param user  Opaque pointer passed through to each callback invocation.
 */
void tether_ble_set_event_cb(tether_ble_event_cb_t cb, void *user)
{
    s_event_cb   = cb;
    s_event_user = user;
}

/**
 * Trigger a fresh scan on the C6 and copy the resulting slim device list into `out`.
 *
 * @param out         Caller-provided array of cap entries.
 * @param cap         Maximum entries to fill.
 * @param count_out   Receives the number filled.
 * @param timeout_ms  Max time to wait for the response.
 * @return            ESP_OK on success, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE on framing
 *                    error, or a slave-side error mapped via status_to_err.
 */
esp_err_t tether_ble_show(tether_dev_t *out, uint8_t cap, uint8_t *count_out,
                          uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(out != NULL && count_out != NULL, ESP_ERR_INVALID_ARG);
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);
    *count_out = 0;

    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_SHOW, NULL, 0, timeout_ms);
    if (r == ESP_OK) {
        r = (s_resp_status == TETHER_ST_OK)
          ? parse_show(s_resp_slot, s_resp_slot_len, out, cap, count_out)
          : status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}

/**
 * Ask the C6 to initiate a connection to the device at `index` in the most recent snapshot.
 * Returns when the C6 acknowledges; actual connection completion arrives via
 * TETHER_BLE_EVT_PAIR_COMPLETE.
 *
 * @param index       Index from a prior tether_ble_show.
 * @param timeout_ms  Max time to wait for the ACK.
 */
esp_err_t tether_ble_pair(uint8_t index, uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);
    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_PAIR, &index, 1, timeout_ms);
    if (r == ESP_OK) {
        r = status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}

esp_err_t tether_ble_pair_by_mac(const uint8_t *mac, uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(mac != NULL, ESP_ERR_INVALID_ARG);
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);
    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_PAIR_MAC, mac, TETHER_MAC_LEN, timeout_ms);
    if (r == ESP_OK) {
        r = status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}

/**
 * Trigger a single-shot scan of one device by index and return its full details
 * (name + raw manufacturer data).
 *
 * @param index       Index from a prior tether_ble_show.
 * @param out         Caller-provided detail struct.
 * @param timeout_ms  Max time to wait (should exceed the slave's single-shot scan window).
 */
esp_err_t tether_ble_read_adv(uint8_t index, tether_adv_detail_t *out, uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(out != NULL, ESP_ERR_INVALID_ARG);
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_READ_ADV, &index, 1, timeout_ms);
    if (r == ESP_OK) {
        r = (s_resp_status == TETHER_ST_OK)
          ? parse_read_adv(s_resp_slot, s_resp_slot_len, out)
          : status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}

/**
 * Enumerate currently connected peers on the C6.
 *
 * @param out         Caller-provided array of cap entries.
 * @param cap         Maximum entries to fill.
 * @param count_out   Receives the number filled (clipped to cap).
 * @param timeout_ms  Max time to wait for the response.
 */
esp_err_t tether_ble_connected(tether_conn_t *out, uint8_t cap, uint8_t *count_out,
                               uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(out != NULL && count_out != NULL, ESP_ERR_INVALID_ARG);
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);
    *count_out = 0;

    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_CONNECTED, NULL, 0, timeout_ms);
    if (r == ESP_OK) {
        r = (s_resp_status == TETHER_ST_OK)
          ? parse_connected(s_resp_slot, s_resp_slot_len, out, cap, count_out)
          : status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}

/**
 * Disconnect the Nth currently-connected peer (same ordering as tether_ble_connected).
 *
 * @param index       Index into the connected list.
 * @param timeout_ms  Max time to wait for the ACK.
 */
esp_err_t tether_ble_disconnect(uint8_t index, uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);
    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_DISCONNECT, &index, 1, timeout_ms);
    if (r == ESP_OK) {
        r = status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}

/**
 * Remove a paired device. Purges the matching tag from the P4's local store by MAC FIRST, then
 * asks the C6 to disconnect the Nth connected peer (OP_REMOVE, which the slave treats like
 * OP_DISCONNECT). Forgetting before disconnecting makes the store the authoritative "removed"
 * state: the P4's reconnect_task gates entirely on the store (both its trigger count and its
 * per-device lookup), so once the tag is gone the device can never be auto-reconnected, even if
 * the disconnect below fails or races an in-flight reconnect attempt.
 *
 * The store purge mirrors what the PAIR_REMOVE event path does for removes initiated on the C6
 * console; here the P4 already holds the MAC, so it deletes locally instead of waiting for the
 * C6 to echo a remove event back.
 *
 * @param index       Index into the connected list (same ordering as tether_ble_connected).
 * @param mac         MAC of the device to purge from the store.
 * @param timeout_ms  Max time to wait for the C6 ACK.
 * @return            ESP_OK if the C6 disconnect was acked; a transport/status error otherwise.
 *                    The store purge happens regardless, so a non-OK return means "forgotten,
 *                    but the disconnect did not confirm."
 */
esp_err_t tether_ble_remove(uint8_t index, const uint8_t mac[TETHER_MAC_LEN],
                            uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(mac != NULL, ESP_ERR_INVALID_ARG);
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);

    /* Forget first: drop the tag from the local store so reconnect_task can never re-pair it
     * (store_tag_delete persists internally). */
    Tag *tag = NULL;
    if (store_tag_find_by_mac(mac, &tag) == ESP_OK && tag != NULL) {
        store_tag_delete(tag->tag_id);
    }

    /* Then ask the C6 to tear down the live connection. */
    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_REMOVE, &index, 1, timeout_ms);
    if (r == ESP_OK) {
        r = status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}

/**
 * Erase the bond for the Nth connected peer on the C6 (OP_REMOVE) WITHOUT touching the P4
 * store. The C6 deletes the peer's LTK/IRK/CSRK from NVS, drops it from the controller
 * resolving list, and terminates the link. Use this when the caller owns the local store
 * itself — e.g. the UI deletes the tag on its own (LVGL) thread and just wants the C6 to
 * forget the bond from a worker task. tether_ble_remove() is the store-managing variant
 * used by the console, where the lib purges the store before issuing OP_REMOVE.
 *
 * @param index       Index into the connected list (same ordering as tether_ble_connected).
 * @param timeout_ms  Max time to wait for the ACK.
 */
esp_err_t tether_ble_unpair(uint8_t index, uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);
    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_REMOVE, &index, 1, timeout_ms);
    if (r == ESP_OK) {
        r = status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}

/**
 * Pull the C6's latest close-and-moving proximity snapshot. Issues OP_DETECT and parses the
 * OP_DETECT_RESP response into *out. The C6 maintains the snapshot continuously, so it replies
 * immediately — no multi-second scan window like tether_ble_show.
 *
 * @param out         Receives the parsed snapshot (count + MAC entries).
 * @param timeout_ms  Max time to wait for the response.
 * @return            ESP_OK on success, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE on framing
 *                    error, or a slave-side error mapped via status_to_err.
 */
esp_err_t tether_ble_detected(tether_scan_report_t *out, uint32_t timeout_ms)
{
    TETHER_BLE_CHECK(out != NULL, ESP_ERR_INVALID_ARG);
    TETHER_BLE_CHECK(s_handle != NULL, ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_request_mtx, portMAX_DELAY);
    esp_err_t r = transact(TETHER_OP_DETECT, NULL, 0, timeout_ms);
    if (r == ESP_OK) {
        r = (s_resp_status == TETHER_ST_OK)
          ? parse_detected(s_resp_slot, s_resp_slot_len, out)
          : status_to_err(s_resp_status);
    }
    xSemaphoreGive(s_request_mtx);
    return r;
}
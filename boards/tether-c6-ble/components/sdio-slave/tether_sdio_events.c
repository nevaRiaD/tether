/*
 * Architecture:
 *
 * This file owns the async-notification side of the P4 <-> C6 SDIO transport. NimBLE GAP
 * callbacks (running on the NimBLE host task) post events here via tether_sdio_post_event();
 * the SDIO task drains the queue once per loop iteration and emits each event as a TLV frame
 * through tether_sdio_send_frame().
 *
 * Decoupling the BLE callback from the SDIO TX path is what keeps the design safe: BLE
 * callbacks must return quickly (they share the NimBLE host task) and must not block on the
 * SDIO driver, but the SDIO TX path is blocking by design (sync send_queue + get_finished).
 * The FreeRTOS queue in the middle absorbs the impedance mismatch.
 *
 * This file handles only the PAIR_COMPLETE and PEER_DISCONN notifications. The proximity
 * snapshot is not an event -- P4 pulls it on demand via the OP_DETECT command and the C6
 * replies with the OP_DETECT_RESP response (see handle_detect in tether_sdio_cmd.c).
 */

#include "tether_sdio_events.h"
#include "tether_sdio_proto.h"
#include "tether_sdio_slave.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"

#include <stdint.h>
#include <string.h>


/* ========== File-scope state ========== */

#ifndef SDIO_EVENT_QUEUE_DEPTH
#define SDIO_EVENT_QUEUE_DEPTH      16
#endif

static const char *TAG = "tether_evt";

static QueueHandle_t s_evt_q = NULL;

/* Per-event scratch for building outbound TLV frames. Lives in its own DMA-attr buffer
 * (separate from the command dispatcher's scratch) so a response in flight doesn't
 * conflict with the next event being assembled. Both go through tether_sdio_send_frame
 * which is single-task synchronous, so they are never truly concurrent. */
DMA_ATTR static uint8_t s_evt_scratch[TETHER_MAX_FRAME_SIZE];


/* ========== Forward declarations ========== */

/* Pack a tether_event_t into s_evt_scratch with the matching opcode + payload and ship it. */
static void send_event_frame(const tether_event_t *evt);


/* ========== Function definitions ========== */

/**
 * Create the FreeRTOS event queue. Must be called before any task can call post_event
 * or drain_events, which in practice means before NimBLE init and before the SDIO task
 * spawns -- tether_sdio_init() handles the ordering.
 */
void tether_sdio_events_init(void)
{
    if (s_evt_q != NULL) {
        return;
    }
    s_evt_q = xQueueCreate(SDIO_EVENT_QUEUE_DEPTH, sizeof(tether_event_t));
    if (s_evt_q == NULL) {
        ESP_LOGE(TAG, "failed to create event queue");
    }
}

/**
 * Enqueue one event. Returns silently on a full queue (events are best-effort).
 *
 * @param evt  Pointer to the event to enqueue. Copied by value into the queue.
 */
void tether_sdio_post_event(const tether_event_t *evt)
{
    if (evt == NULL || s_evt_q == NULL) {
        return;
    }

    if (xQueueSend(s_evt_q, evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full; dropping kind=%d", (int)evt->kind);
    }
}

/**
 * Drain every queued event in arrival order, emitting each as a TLV frame. Returns once
 * the queue is empty. Runs in the SDIO task and therefore holds the single-task TX
 * invariant for the duration -- safe to share the slave's send path.
 */
void tether_sdio_drain_events(void)
{
    if (s_evt_q == NULL) {
        return;
    }
    tether_event_t evt;
    while (xQueueReceive(s_evt_q, &evt, 0) == pdTRUE) {
        send_event_frame(&evt);
    }
}

/**
 * Serialize one event into the dedicated event scratch and send it via tether_sdio_send_frame.
 * The opcode + payload layout for each event kind is defined in tether_sdio_proto.h.
 *
 * @param evt  Event to serialize. Must not be NULL.
 */
static void send_event_frame(const tether_event_t *evt)
{
    uint8_t  op    = 0;
    uint16_t plen  = 0;
    uint8_t  payload[TETHER_MAX_PAYLOAD_SIZE];

    switch (evt->kind) {
    case TETHER_EVT_KIND_PAIR_COMPLETE:
        op   = TETHER_EVT_PAIR_COMPLETE;
        memcpy(&payload[0], evt->u.pair_complete.mac, TETHER_MAC_LEN);
        payload[TETHER_MAC_LEN] = evt->u.pair_complete.success;
        payload[TETHER_MAC_LEN + sizeof(uint8_t)] = (uint8_t)evt->u.pair_complete.connection_type;
        plen = TETHER_MAC_LEN + 2;
        break;

    case TETHER_EVT_KIND_PEER_DISCONN:
        op   = TETHER_EVT_PEER_DISCONN;
        memcpy(&payload[0], evt->u.peer_disconn.mac, TETHER_MAC_LEN);
        memcpy(&payload[TETHER_MAC_LEN], &evt->u.peer_disconn.reason, sizeof(uint16_t));
        payload[TETHER_MAC_LEN + sizeof(uint16_t)] = evt->u.peer_disconn.intentional ? 1 : 0;
        plen = TETHER_MAC_LEN + sizeof(uint16_t) + 1;
        break;

    case TETHER_EVT_KIND_REMOVE:
        op   = TETHER_EVT_PAIR_REMOVE;
        memcpy(&payload[0], evt->u.remove.mac, TETHER_MAC_LEN);
        plen = TETHER_MAC_LEN;
        break;

    default:
        ESP_LOGW(TAG, "unknown event kind %d", (int)evt->kind);
        return;
    }

    tether_frame_hdr_t hdr = {
        .op     = op,
        .status = TETHER_ST_OK,
        .flags  = 0,
        .len    = plen,
    };
    memcpy(s_evt_scratch, &hdr, sizeof hdr);
    if (plen > 0) {
        memcpy(s_evt_scratch + sizeof hdr, payload, plen);
    }
    esp_err_t r = tether_sdio_send_frame(s_evt_scratch, sizeof hdr + plen);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "send_frame failed: 0x%x", r);
    }
}

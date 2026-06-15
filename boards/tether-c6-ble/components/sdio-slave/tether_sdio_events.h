#ifndef TETHER_SDIO_EVENTS_H
#define TETHER_SDIO_EVENTS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    TETHER_EVT_KIND_PAIR_COMPLETE = 1,
    TETHER_EVT_KIND_PEER_DISCONN  = 2,
    TETHER_EVT_KIND_REMOVE        = 3,
} tether_evt_kind_t;

typedef enum {
    TETHER_CENT_TO_PERIPHERAL = 1,
    TETHER_PERIPHERAL_TO_CENT = 2,
} tether_conn_type_t;

typedef struct {
    tether_evt_kind_t kind;
    union {
        struct {
            uint8_t             mac[6];
            uint8_t             success;            /* 0 = failed, nonzero = connected ok */
            uint8_t             connection_type;    /* 1 = tether->peripheral */
                                                    /* 2 = peripheral->tether */
        } pair_complete;
        struct {
            uint8_t  mac[6];
            uint16_t reason;            /* HCI disconnect reason */
            bool intentional;           /* checks if disconn was activated by command */
        } peer_disconn;
        struct {
            uint8_t mac[6];             /* device to purge from the P4 store */
        } remove;
    } u;
} tether_event_t;


/* Create the event queue. Call once from tether_sdio_init() before the task is spawned. */
void tether_sdio_events_init(void);

/* Push one event onto the queue, non-blocking. Safe to call from any task context.
 * Returns silently on a full queue (events are best-effort). */
void tether_sdio_post_event(const tether_event_t *evt);

/* Drain every queued event and emit each as a TLV frame via tether_sdio_send_frame(). Called
 * from the SDIO task; runs to completion in the caller's context (single-task ownership). */
void tether_sdio_drain_events(void);


#ifdef __cplusplus
}
#endif

#endif /* TETHER_SDIO_EVENTS_H */

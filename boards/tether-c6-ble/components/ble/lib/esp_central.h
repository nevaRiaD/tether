/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef H_ESP_CENTRAL_
#define H_ESP_CENTRAL_

#include <stdint.h>
#include <stdbool.h>
#include "sys/queue.h"
#include "syscfg/syscfg.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "modlog/modlog.h"
#include "tether_slave_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PEER_ADDR_VAL_SIZE  6
#define PEER_RSSI_BUFF_SIZE MAX_RSSI_BUFF_SIZE
#define DEVICE_TYPE_RSSI    0
#define DEVICE_TYPE_ACCEL   1

/** Misc. */
void print_bytes(const uint8_t *bytes, int len);
void print_mbuf(const struct os_mbuf *om);
void print_mbuf_data(const struct os_mbuf *om);
char *addr_str(const void *addr);
void print_uuid(const ble_uuid_t *uuid);
void print_conn_desc(const struct ble_gap_conn_desc *desc);
void print_adv_fields(const struct ble_hs_adv_fields *fields);
void ext_print_adv_report(const void *param);

/** Peer. */
struct peer_dsc {
    SLIST_ENTRY(peer_dsc) next;
    struct ble_gatt_dsc dsc;
};
SLIST_HEAD(peer_dsc_list, peer_dsc);

struct peer_chr {
    SLIST_ENTRY(peer_chr) next;
    struct ble_gatt_chr chr;

    struct peer_dsc_list dscs;
};
SLIST_HEAD(peer_chr_list, peer_chr);
SLIST_HEAD(peer_svc_list, peer_svc);

#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
struct peer_incl_svc {
    SLIST_ENTRY(peer_incl_svc) next;
    struct ble_gatt_incl_svc svc;
};
SLIST_HEAD(peer_incl_svc_list, peer_incl_svc);
#endif

struct peer_svc {
    SLIST_ENTRY(peer_svc) next;
    struct ble_gatt_svc svc;
#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    struct peer_incl_svc_list incl_svc;
#endif
    struct peer_chr_list chrs;
};

struct peer;
typedef void peer_disc_fn(const struct peer *peer, int status, void *arg);

/**
 * @brief The callback function for the devices traversal.
 *
 * @param peer
 * @param arg
 * @return int  0, continue; Others, stop the traversal.
 *
 */
typedef int peer_traverse_fn(const struct peer *peer, void *arg);

struct peer {
    SLIST_ENTRY(peer) next;
    uint16_t conn_handle;

    uint8_t peer_addr[PEER_ADDR_VAL_SIZE];

    /* type to decide if accelerometer or rssi
     *
     * #define DEVICE_TYPE_RSSI    0
     * #define DEVICE_TYPE_ACCEL   1
     */
    uint8_t device_type;

    /** Accelerometer-tag motion state, pushed by the tag over GATT notifications.
     *  accel_moving        : latest motion flag (0/1). Written from the NimBLE host
     *                        task (BLE_GAP_EVENT_NOTIFY_RX) and read from the scan
     *                        task (ble_accel_parse); volatile for cross-task
     *                        visibility. Single-byte access is atomic, so no lock.
     *  accel_chr_val_handle: value handle of the motion characteristic, captured at
     *                        subscribe time and used to validate incoming notifications. */
    volatile uint8_t accel_moving;
    uint16_t         accel_chr_val_handle;

    /** Set true by the OP_DISCONNECT/OP_REMOVE command path right before
     *  ble_gap_terminate(), so BLE_GAP_EVENT_DISCONNECT can tell the P4 the
     *  drop was deliberate (PEER_DISCONN `intentional` byte). Auto-cleared on
     *  reuse (peer_add memsets the struct) and consumed when the peer is
     *  deleted in the disconnect handler. */
    bool intentional_disconnect;

    /** RSSI history for averaging and motion detection. */
    int8_t  rssi_buff[PEER_RSSI_BUFF_SIZE];
    uint8_t rssi_head;
    uint8_t rssi_tail;

#ifdef T_BLE_TEST_RSSI_TRACE
    /** Fixed-window RSSI trace test state. */
    uint32_t rssi_trace_generation;
    uint32_t rssi_trace_arm_ms;
    uint32_t rssi_trace_start_ms;
    uint16_t rssi_trace_count;
    int8_t   rssi_trace_countdown_last_s;
    bool     rssi_trace_started;
    bool     rssi_trace_done;
    uint16_t rssi_trace_ms[RSSI_TRACE_MAX_SAMPLES];
    int8_t   rssi_trace_values[RSSI_TRACE_MAX_SAMPLES];
#endif

#ifdef T_BLE_TEST_RSSI_ROLLING
    /** 2-state Kalman motion detection state (level + velocity). */
    float    rssi_kf_level;            /* smoothed RSSI estimate (dBm) */
    float    rssi_kf_velocity;         /* smoothed RSSI velocity (dBm/s) */
    float    rssi_kf_P[4];            /* 2×2 covariance matrix, row-major: [P00,P01,P10,P11] */
    uint32_t rssi_kf_last_ms;         /* timestamp of last Kalman update */
    bool     rssi_kf_initialized;     /* false until first sample */
    float    rssi_kf_level_baseline;  /* slow EMA of Kalman level — drift reference */
    float    rssi_motion_score;       /* leaky bucket score for motion detection */
    bool     rssi_motion_moving;      /* current declared motion state */
    bool     rssi_close;              /* hysteretic CLOSE state (enter at -57, exit at -67) */
    uint8_t  rssi_log_skip;           /* rate-limit debug log output */
#endif

    /** List of discovered GATT services. */
    struct peer_svc_list svcs;

    /** Keeps track of where we are in the service discovery process. */
    uint16_t disc_prev_chr_val;
    struct peer_svc *cur_svc;

    /** Callback that gets executed when service discovery completes. */
    peer_disc_fn *disc_cb;
    void *disc_cb_arg;
};

void peer_traverse_all(peer_traverse_fn *trav_cb, void *arg);

int peer_disc_svc_by_uuid(uint16_t conn_handle, const ble_uuid_t *uuid, peer_disc_fn *disc_cb,
                          void *disc_cb_arg);

int peer_disc_all(uint16_t conn_handle, peer_disc_fn *disc_cb,
                  void *disc_cb_arg);
const struct peer_dsc *
peer_dsc_find_uuid(const struct peer *peer, const ble_uuid_t *svc_uuid,
                   const ble_uuid_t *chr_uuid, const ble_uuid_t *dsc_uuid);
const struct peer_chr *
peer_chr_find_uuid(const struct peer *peer, const ble_uuid_t *svc_uuid,
                   const ble_uuid_t *chr_uuid);
const struct peer_svc *
peer_svc_find_uuid(const struct peer *peer, const ble_uuid_t *uuid);
int peer_delete(uint16_t conn_handle);
int peer_add(uint16_t conn_handle, uint8_t device_type);

/**
 * @brief Map an advertised device name to a DEVICE_TYPE_* value.
 *
 * Names beginning with "Tether Tag" are accelerometer tags; everything else,
 * including a NULL or empty name, is treated as an RSSI tag.
 *
 * @param name  Advertised device name (may be NULL).
 * @return      DEVICE_TYPE_ACCEL for accelerometer tags, otherwise DEVICE_TYPE_RSSI.
 */
int ble_device_type_from_name(const char *name);
#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
int peer_init(int max_peers, int max_svcs, int max_incl_svcs, int max_chrs, int max_dscs);
#else
int peer_init(int max_peers, int max_svcs, int max_chrs, int max_dscs);
#endif
struct peer *
peer_find(uint16_t conn_handle);
#if MYNEWT_VAL(ENC_ADV_DATA)
int peer_set_addr(uint16_t conn_handle, uint8_t *peer_addr);
#endif
#ifdef __cplusplus
}
#endif

#endif

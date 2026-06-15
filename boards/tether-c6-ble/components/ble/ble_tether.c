#include "ble_tether.h"
#include "ble_rssi_buff.h"
#include "ble_pair.h"
#include "device_scan.h"
#include "esp_central.h"
#include "gatt_svr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "tether_sdio_events.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <stdint.h>
#include <string.h>

void ble_store_config_init(void);

static const char *TAG = "TETHER";

static const ble_uuid_t *remote_svc_uuid =
    BLE_UUID128_DECLARE(0x2d, 0x71, 0xa2, 0x59, 0xb4, 0x58, 0xc8, 0x12,
                        0x99, 0x99, 0x43, 0x95, 0x12, 0x2f, 0x46, 0x59);

/* Dedicated characteristic on the accel tag that carries the motion flag (0/1),
 * with a CCCD (0x2902) the C6 writes to enable notifications. Canonical UUID
 * "33333333-2222-2222-1111-111100000001" (bytes below are little-endian, i.e.
 * the printed string reversed). Distinct from the tag's message characteristic
 * (…00000000) so write-messages and motion-notifies don't share one attribute.
 * Must match MOTION_CHAR_UUID in the tag's Arduino firmware. */
static const ble_uuid_t *remote_chr_uuid =
    BLE_UUID128_DECLARE(0x01, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11,
                        0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33);

static uint8_t ext_adv_pattern_1[] = {
    0x02, BLE_HS_ADV_TYPE_FLAGS, 0x06,
    0x03, BLE_HS_ADV_TYPE_COMP_UUIDS16, 0x12, 0x18, /* HID Service UUID 0x1812 */
    0x03, BLE_HS_ADV_TYPE_APPEARANCE, 0xC1, 0x03,          /* Keyboard 0x03C1 */
    0x07, BLE_HS_ADV_TYPE_COMP_NAME, 'T', 'e', 't', 'h', 'e', 'r',
};

static uint8_t s_ble_multi_conn_num = 0;

/* Auto-reconnect is handled by the P4's reconnect_task, which checks the real
 * tag store. The C6 no longer auto-reconnects from the scan callback — NimBLE's
 * bond store is not the right source of truth (it records ANY device ever seen). */

/* Deferred peripheral-side connection-parameter update.
 * iOS rejects an immediate request with HCI 0x2A (Different Transaction Collision)
 * because service discovery / MTU exchange / pairing run right after connect.
 * We delay the request a couple seconds so those finish first.
 *
 * Each pending update gets its own one-shot timer so multiple near-simultaneous
 * connections (e.g. phone + tag) don't clobber each other.
 */
#define CONN_UPDATE_DELAY_US        (2 * 1000 * 1000)
#define MAX_PENDING_CONN_UPDATES    4

struct pending_conn_update {
    esp_timer_handle_t timer;
    uint16_t           conn_handle;
    bool               in_use;
};
static struct pending_conn_update s_pending_updates[MAX_PENDING_CONN_UPDATES];

static void conn_update_timer_cb(void *arg)
{
    struct pending_conn_update *slot = (struct pending_conn_update *)arg;
    uint16_t handle = slot->conn_handle;
    slot->in_use = false;

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(handle, &desc) != 0) {
        return;
    }

    struct ble_gap_upd_params upd = {
        .itvl_min = BLE_GAP_CONN_ITVL_MS(30),
        .itvl_max = BLE_GAP_CONN_ITVL_MS(50),
        .latency = 0,
        .supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(10000),
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(handle, &upd);
    if (rc != 0) {
        ESP_LOGW(TAG, "conn_update failed; handle=%u rc=%d", handle, rc);
    } 
    else {
        ESP_LOGI(TAG, "conn_update sent; handle=%u itvl=30-50ms supv=10000ms", handle);
    }
}

static void schedule_conn_update(uint16_t conn_handle)
{
    struct pending_conn_update *slot = NULL;
    for (int i = 0; i < MAX_PENDING_CONN_UPDATES; i++) {
        if (!s_pending_updates[i].in_use) {
            slot = &s_pending_updates[i];
            break;
        }
    }
    if (slot == NULL) {
        ESP_LOGW(TAG, "No free conn_update slot; handle=%u update skipped", conn_handle);
        return;
    }

    if (slot->timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = conn_update_timer_cb,
            .arg = slot,
            .name = "conn_upd",
        };
        if (esp_timer_create(&args, &slot->timer) != ESP_OK) {
            ESP_LOGW(TAG, "esp_timer_create failed; conn_update will not run");
            return;
        }
    }
    slot->conn_handle = conn_handle;
    slot->in_use = true;
    esp_timer_start_once(slot->timer, CONN_UPDATE_DELAY_US);
}


/* ========== Internal Function Declarations ========== */

/* Service-discovery completion callback for connections this device initiated.          */
static void ble_central_on_disc_complete(const struct peer *peer, int status, void *arg);

/* GATT write-completion callback for the accel motion CCCD subscription.                 */
static int  ble_accel_subscribe_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                   struct ble_gatt_attr *attr, void *arg);

/* GAP event callback for connections initiated by this device (central role).           */
static int  ble_central_client_gap_event(struct ble_gap_event *event, void *arg);

/* GAP event callback for connections accepted by this device (peripheral role).         */
static int  ble_central_server_gap_event(struct ble_gap_event *event, void *arg);

/* Start connectable extended advertising on instance 0 if not already active.           */
static void ble_central_advertise(void);

/* Start GAP extended discovery on both uncoded and coded PHYs.                          */
/* ble_tether_start_scan / ble_tether_stop_scan are declared in ble_tether.h; defined below. */
static void ble_start_scan_internal(uint16_t duration_10ms, bool aggressive);

/* Initiate a multi-PHY connection to the advertiser at `peer_addr`.                     */
static int ble_central_connect(const ble_addr_t *peer_addr);

/* NimBLE host reset callback.                                                           */
static void blecent_on_reset(int reason);

/* NimBLE host sync callback; configures scheduling and starts advertising/scanning.     */
static void blecent_on_sync(void);

/* FreeRTOS task that runs the NimBLE host event loop.                                   */
static void blecent_host_task(void *param);


/* ========== Function definitions ========== */

/**
 * Called when service discovery of the specified peer has completed.
 */
static void ble_central_on_disc_complete(const struct peer *peer, int status, void *arg)
{

    if (status != 0) {
        /* Service discovery failed.  Terminate the connection. */
        ESP_LOGE(TAG, "Error: Service discovery failed; status=%d conn_handle=%d", status,
                 peer->conn_handle);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    /* Accel tags push their motion flag (0/1) via notifications; subscribe by writing
     * the Client Characteristic Configuration Descriptor (CCCD). RSSI tags carry no such
     * characteristic — their motion is inferred C6-side from RSSI variance. */
    if (peer->device_type == DEVICE_TYPE_ACCEL) {
        /* Find the motion characteristic and its CCCD (0x2902) under the Tether service. */
        const struct peer_chr *chr =
            peer_chr_find_uuid(peer, remote_svc_uuid, remote_chr_uuid);
        const struct peer_dsc *cccd =
            peer_dsc_find_uuid(peer, remote_svc_uuid, remote_chr_uuid,
                               BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16));
        if (chr == NULL || cccd == NULL) {
            ESP_LOGE(TAG, "Accel motion chr/CCCD not found; conn_handle=%d — cannot subscribe",
                     peer->conn_handle);
            return;
        }

        /* Remember the value handle so NOTIFY_RX can confirm an incoming notification
         * is the motion characteristic. peer is const here, but this caches
         * discovery-derived state on it, like the RSSI path does. */
        ((struct peer *)peer)->accel_chr_val_handle = chr->chr.val_handle;

        /* Write 0x0001 (little-endian) to the CCCD to enable notifications. */
        static const uint8_t notify_en[2] = { 0x01, 0x00 };
        int rc = ble_gattc_write_flat(peer->conn_handle, cccd->dsc.handle,
                                      notify_en, sizeof notify_en,
                                      ble_accel_subscribe_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to write accel CCCD; conn_handle=%d rc=%d",
                     peer->conn_handle, rc);
        }
    }

    /* Service discovery has completed successfully.  Now we have a complete
     * list of services, characteristics, and descriptors that the peer
     * supports.
     */
    ESP_LOGD(TAG, "Service discovery complete; status=%d conn_handle=%d\n", status,
             peer->conn_handle);
}

/**
 * Called when the CCCD write that enables accel motion notifications completes.
 * Logs the outcome; on failure the C6 simply won't receive motion updates for
 * this tag. The signature is fixed by NimBLE's ble_gatt_attr_fn typedef, so
 * `attr` (the written attribute, not needed here) and `arg` (our unused cb_arg)
 * are required parameters even though we don't read them.
 */
static int ble_accel_subscribe_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;

    if (error == NULL || error->status != 0) {
        ESP_LOGE(TAG, "Accel subscribe failed; conn_handle=%d status=%d",
                 conn_handle, error ? error->status : -1);
    } 
    else {
        ESP_LOGI(TAG, "Accel notifications enabled; conn_handle=%d", conn_handle);
    }
    return 0;
}


/**
 * The nimble host executes this callback when a GAP event occurs.  The application associates a GAP
 * event callback with each connection that is established. This callback will be only used by the
 * central.
 *
 * @param event                 The event being signalled.
 * @param arg                   Application-specified argument; unused by
 *                                  blecent.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int ble_central_client_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_hs_adv_fields fields;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_EXT_DISC:
        rc = ble_hs_adv_parse_fields(&fields, event->ext_disc.data, event->ext_disc.length_data);

        /* Record every advertiser so ble_show_adv_devices() can return them. */
        if (rc == 0) {
            ble_pair_record_adv(&event->ext_disc.addr, fields.name, fields.name_len, fields.mfg_data, fields.mfg_data_len, event->ext_disc.rssi);
        } 
        else {
            ble_pair_record_adv(&event->ext_disc.addr, NULL, 0, NULL, 0, event->ext_disc.rssi);
        }
        /* SCAN_REPORT is NOT posted from the advertising path. It fires from the proximity
         * detector when the close-and-moving set of CONNECTED peers changes. */

#ifdef BLE_AUTOCONNECT_ENABLED
        /* An advertisement report was received during GAP discovery. */
        if ((rc == 0) && fields.name && (fields.name_len >= strlen(BLE_PEER_NAME)) &&
            !strncmp((const char *)fields.name, BLE_PEER_NAME, strlen(BLE_PEER_NAME))) {
            ble_central_connect(&event->ext_disc.addr);
        }
#endif

        /* Auto-reconnect is handled by the P4 (reconnect_task checks the real
         * tag store). The C6 just records advertisers for ble_show. */

        return 0;
    case BLE_GAP_EVENT_CONNECT: {
        struct ble_gap_conn_desc desc;
        tether_event_t evt = {
            .kind = TETHER_EVT_KIND_PAIR_COMPLETE,
            .u.pair_complete = { 
                .success = (event->connect.status == 0) ? 1 : 0,
                .connection_type = TETHER_CENT_TO_PERIPHERAL,
            },
        };

        /* Attempt to establish connection for Central */
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "Central: Connection failed; status=0x%x\n", event->connect.status);
            tether_sdio_post_event(&evt);
            /* The connect attempt cancelled scanning; resume the low-duty background
             * scan so OP_SHOW discovery and the P4's reconnect_task keep working. */
            ble_start_scan_internal(0, false);
            return 1;
        }
        ESP_LOGI(TAG, "Connection established. Handle:%d, Total:%d", event->connect.conn_handle,
                     ++s_ble_multi_conn_num);

        /* Resolve the peer's MAC and advertised name so we can classify the
            * tag (accel vs RSSI) before recording it. */
        uint8_t device_type = DEVICE_TYPE_RSSI;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
            memcpy(evt.u.pair_complete.mac, desc.peer_id_addr.val, 6);

            char name[ADV_NAME_MAX];
            ble_pair_find_name_by_mac(desc.peer_id_addr.val, name, sizeof name);
            device_type = ble_device_type_from_name(name);
        }

        /* Remember peer. */
        rc = peer_add(event->connect.conn_handle, device_type);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to add peer; rc=%d\n", rc);
        } 
        else {
            /* Perform service discovery */
            rc = peer_disc_svc_by_uuid(event->connect.conn_handle, remote_svc_uuid,
                                    ble_central_on_disc_complete, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to discover services; rc=%d\n", rc);
            }
        }

        /* Tighten connection parameters now that we're connected.
            * Initial params are set for 30 peers (145 ms interval) — too slow
            * for 2-3 tags, and the long interval makes supervision timeouts
            * more likely when moving. Defer 2 s so service discovery finishes. */
        schedule_conn_update(event->connect.conn_handle);

        /* Initiating the connection cancelled scanning. Resume the low-duty
         * background scan so discovery (OP_SHOW) and proximity advertisers keep
         * being recorded while this (and any other) connection stays up. BLE
         * supports concurrent scan + connection; the gentle duty cycle limits
         * radio contention on cheap tags. */
        ble_start_scan_internal(0, false);

        tether_sdio_post_event(&evt);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        /* Connection terminated. */
        print_conn_desc(&event->disconnect.conn);

        tether_event_t evt = {
            .kind = TETHER_EVT_KIND_PEER_DISCONN,
            .u.peer_disconn = { .reason = (uint16_t)event->disconnect.reason },
        };
        memcpy(evt.u.peer_disconn.mac, event->disconnect.conn.peer_id_addr.val, 6);

        /* Tell the P4 whether the C6 tore this link down on purpose (OP_DISCONNECT/
         * OP_REMOVE set this flag before ble_gap_terminate). Defaults to 0 for
         * supervision timeouts, range loss, etc. Cleared when the peer is deleted. */
        struct peer *dp = peer_find(event->disconnect.conn.conn_handle);
        evt.u.peer_disconn.intentional = (dp != NULL && dp->intentional_disconnect);

        tether_sdio_post_event(&evt);

        /* Forget about peer. */
        peer_delete(event->disconnect.conn.conn_handle);

        --s_ble_multi_conn_num;
        ESP_LOGI(TAG, "Central disconnected; Handle:%d, Reason=%d, Total:%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason,
                 s_ble_multi_conn_num);

        /* Resume the low-duty background scan unconditionally — even with other
         * connections still up. Discovery (OP_SHOW) and the P4's reconnect_task
         * must keep seeing advertisers while ≥1 tag is connected; the gentle duty
         * cycle keeps radio contention low on cheap BLE tags. */
        ble_start_scan_internal(0, false);
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE: {
        ESP_LOGI(TAG, "discovery complete; reason=%d\n", event->disc_complete.reason);
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        /* Accel tags push their motion flag (0/1) here. Cache it on the peer so the
         * scan task (ble_accel_parse) can read it without a GATT round-trip. The
         * cached byte is volatile; single-byte write needs no lock. */
        struct peer *peer = peer_find(event->notify_rx.conn_handle);
        if (peer == NULL || peer->device_type != DEVICE_TYPE_ACCEL) {
            return 0;
        }

        /* Ignore notifications from any characteristic other than the motion one. */
        if (event->notify_rx.attr_handle != peer->accel_chr_val_handle) {
            return 0;
        }

        uint8_t motion = 0;
        if (event->notify_rx.om != NULL &&
            os_mbuf_copydata(event->notify_rx.om, 0, 1, &motion) == 0) {
            peer->accel_moving = motion ? 1 : 0;
        }
        return 0;
    }
#if MYNEWT_VAL(BLE_POWER_CONTROL)
    case BLE_GAP_EVENT_TRANSMIT_POWER: {
        ESP_LOGD(TAG, "Transmit power event : status=%d conn_handle=%d reason=%d phy=%d "
                 "power_level=%x power_level_flag=%d delta=%d", event->transmit_power.status,
                 event->transmit_power.conn_handle, event->transmit_power.reason,
                 event->transmit_power.phy, event->transmit_power.transmit_power_level,
                 event->transmit_power.transmit_power_level_flag, event->transmit_power.delta);
        return 0;
    }
    case BLE_GAP_EVENT_PATHLOSS_THRESHOLD: {
        ESP_LOGD(TAG, "Pathloss threshold event : conn_handle=%d current path loss=%d "
                 "zone_entered =%d", event->pathloss_threshold.conn_handle,
                 event->pathloss_threshold.current_path_loss, event->pathloss_threshold.zone_entered);
        return 0;
    }
#endif
    default: {
        return 0;
    }
    }
}

/**
 * The nimble host executes this callback when a GAP event occurs.  The application associates a GAP
 * event callback with each connection that is established. This callback will be only used by the
 * peripheral.
 *
 * @param event                 The event being signalled.
 * @param arg                   Application-specified argument; unused by
 *                                  blecent.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int ble_central_server_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        /* The connectable adv has been established. We will act as the peripheral. */
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "Peripheral: Connection failed; status=0x%x\n", event->connect.status);
            return 1;
        }
        
        ESP_LOGI(TAG, "Peripheral connected to central. Handle:%d, Total:%d",
                    event->connect.conn_handle, ++s_ble_multi_conn_num);

        /* Resolve the peer's MAC and advertised name so we can classify the
            * tag (accel vs RSSI) before recording it. */
        struct ble_gap_conn_desc desc;
        int8_t  rssi        = 0;
        uint8_t device_type = DEVICE_TYPE_RSSI;
        bool    have_desc   = (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0);
        if (have_desc) {
            char name[ADV_NAME_MAX];
            ble_pair_find_name_by_mac(desc.peer_id_addr.val, name, sizeof name);
            device_type = ble_device_type_from_name(name);
        }

        int rc = peer_add(event->connect.conn_handle, device_type);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to add peer; rc=%d", rc);
        }

        if (have_desc && ble_gap_conn_rssi(event->connect.conn_handle, &rssi) == 0) {
            ESP_LOGI(TAG, "Peer ID Address: %02X:%02X:%02X:%02X:%02X:%02X  RSSI: %d",
                desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
                desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
                rssi);
            ESP_LOGI(TAG, "Negotiated params: itvl=%u (%.2f ms), latency=%u, supervision_timeout=%u (%u ms)",
                desc.conn_itvl, desc.conn_itvl * 1.25,
                desc.conn_latency,
                desc.supervision_timeout, desc.supervision_timeout * 10);
        }

        /* Defer the conn-update by a couple seconds. iOS rejects an immediate
            * request with HCI 0x2A because its own initial procedures (service
            * discovery, MTU exchange, pairing) are still running.
            */
        schedule_conn_update(event->connect.conn_handle);
        ble_gap_security_initiate(event->connect.conn_handle);
        ble_central_advertise();
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "Conn updated; Handle:%d status=%d -> itvl=%u (%.2f ms), latency=%u, supervision_timeout=%u (%u ms)",
                     event->conn_update.conn_handle, event->conn_update.status,
                     desc.conn_itvl, desc.conn_itvl * 1.25,
                     desc.conn_latency,
                     desc.supervision_timeout, desc.supervision_timeout * 10);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        tether_event_t evt = {
            .kind = TETHER_EVT_KIND_PEER_DISCONN,
            .u.peer_disconn = { .reason = (uint16_t)event->disconnect.reason },
        };
        memcpy(evt.u.peer_disconn.mac, event->disconnect.conn.peer_id_addr.val, 6);

        /* Tell the P4 whether the C6 tore this link down on purpose (OP_DISCONNECT/
         * OP_REMOVE set this flag before ble_gap_terminate). Defaults to 0 for
         * supervision timeouts, range loss, etc. Cleared when the peer is deleted. */
        struct peer *dp = peer_find(event->disconnect.conn.conn_handle);
        evt.u.peer_disconn.intentional = (dp != NULL && dp->intentional_disconnect);

        tether_sdio_post_event(&evt);

        peer_delete(event->disconnect.conn.conn_handle);
        ESP_LOGI(TAG, "Peripheral disconnected; Handle:%d, Reason=%d, Total:%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason,
                 --s_ble_multi_conn_num);
        ble_central_advertise();
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;

        /* Add to event-queue for P4 to receive peri->central evt */
        tether_event_t evt = {
            .kind = TETHER_EVT_KIND_PAIR_COMPLETE,
            .u.pair_complete = {
                .success = (event->enc_change.status == 0) ? 1 : 0,
                .connection_type = TETHER_PERIPHERAL_TO_CENT,
            },
        };

        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0) {
            ESP_LOGE(TAG, "Could not find connection handle for BLE_GAP_EVENT_ENC_CHANGE");
            evt.u.pair_complete.success = 0;
            tether_sdio_post_event(&evt);
            return 1;
        }
        memcpy(evt.u.pair_complete.mac, desc.peer_id_addr.val, 6);

        ESP_LOGI(TAG, "Encryption %s; handle=%d bonded=%d authenticated=%d",
                 event->enc_change.status == 0 ? "enabled" : "failed",
                 event->enc_change.conn_handle,
                 desc.sec_state.bonded,
                 desc.sec_state.authenticated);

        tether_sdio_post_event(&evt);
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        ESP_LOGI(TAG, "Repeat pairing; deleted old bond");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

#if MYNEWT_VAL(BLE_POWER_CONTROL)
    case BLE_GAP_EVENT_TRANSMIT_POWER:
        ESP_LOGD(TAG, "Transmit power event : status=%d conn_handle=%d reason=%d phy=%d "
                 "power_level=%x power_level_flag=%d delta=%d", event->transmit_power.status,
                 event->transmit_power.conn_handle, event->transmit_power.reason,
                 event->transmit_power.phy, event->transmit_power.transmit_power_level,
                 event->transmit_power.transmit_power_level_flag, event->transmit_power.delta);
        return 0;

    case BLE_GAP_EVENT_PATHLOSS_THRESHOLD:
        ESP_LOGD(TAG, "Pathloss threshold event : conn_handle=%d current path loss=%d "
                 "zone_entered =%d", event->pathloss_threshold.conn_handle,
                 event->pathloss_threshold.current_path_loss, event->pathloss_threshold.zone_entered);
        return 0;
#endif

    default:
        return 0;
    }
}

/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void ble_central_advertise(void)
{
    int rc;
    struct ble_gap_ext_adv_params params;
    struct os_mbuf *data;
    uint8_t instance = 0;

    /* First check if any instance is already active */
    if (ble_gap_ext_adv_active(instance)) {
        return;
    }

    memset (&params, 0, sizeof(params));

    /* Enable connectable advertising */
    params.connectable = 1;

#ifdef T_BLE_LEGACY_MODE 
    params.scannable = 1;
    params.legacy_pdu = 1;
#else
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    params.sid = 1;
#endif
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.tx_power = 127;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(300);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(300);

    rc = ble_gap_ext_adv_configure(instance, &params, NULL,
                                   ble_central_server_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ext_adv_configure failed; rc=%d", rc);
        return;
    }

    /* Get mbuf for adv data */
    data = os_msys_get_pkthdr(sizeof(ext_adv_pattern_1), 0);
    if (data == NULL) {
        ESP_LOGW(TAG, "os_msys_get_pkthdr returned NULL (out of mbufs?)");
        return;
    }
    rc = os_mbuf_append(data, ext_adv_pattern_1, sizeof(ext_adv_pattern_1));
    if (rc != 0) {
        ESP_LOGW(TAG, "os_mbuf_append failed; rc=%d", rc);
        os_mbuf_free_chain(data);
        return;
    }

    rc = ble_gap_ext_adv_set_data(instance, data);
    if (rc != 0) {
        ESP_LOGW(TAG, "ext_adv_set_data failed; rc=%d", rc);
        os_mbuf_free_chain(data);
        return;
    }

    /* Start advertising */
    rc = ble_gap_ext_adv_start(instance, 0, 0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ext_adv_start failed; rc=%d", rc);
        return;
    }
}

/**
 * Internal: start scanning with a given duration (in 10ms units, 0 = forever).
 * @param aggressive  true  = discovery mode (wide window, catches names reliably)
 *                    false = background mode (narrow window, protects connections)
 */
static void ble_start_scan_internal(uint16_t duration_10ms, bool aggressive)
{
    int rc;

    if (ble_gap_disc_active()) {
        return;
    }

    struct ble_gap_ext_disc_params uncoded_disc_params;
    struct ble_gap_ext_disc_params coded_disc_params;

    if (aggressive) {
        /* Discovery / pairing: need to reliably catch advertisement + scan response.
         * 60 % duty — fine because discovery is short-lived and user-initiated. */
        uncoded_disc_params.passive = 0;
        uncoded_disc_params.itvl   = BLE_GAP_SCAN_ITVL_MS(200);
        uncoded_disc_params.window = BLE_GAP_SCAN_WIN_MS(120);

        coded_disc_params.passive = 0;
        coded_disc_params.itvl   = BLE_GAP_SCAN_ITVL_MS(200);
        coded_disc_params.window = BLE_GAP_SCAN_WIN_MS(120);
    } 
    else {
        /* Background reconnect: keep duty low so existing connections aren't starved. */
        uncoded_disc_params.passive = 0;
        uncoded_disc_params.itvl   = BLE_GAP_SCAN_ITVL_MS(800);
        uncoded_disc_params.window = BLE_GAP_SCAN_WIN_MS(80);

        coded_disc_params.passive = 0;
        coded_disc_params.itvl   = BLE_GAP_SCAN_ITVL_MS(800);
        coded_disc_params.window = BLE_GAP_SCAN_WIN_MS(100);
    }

    rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, duration_10ms, 0, 0, 0, 0,
                          &uncoded_disc_params, &coded_disc_params,
                          ble_central_client_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error initiating GAP discovery procedure; rc=%d\n", rc);
    }
}

/**
 * Start aggressive indefinite scan (for SHOW / pairing discovery).
 * Caller must call stop_scan when done.
 */
void ble_tether_start_scan(void)
{
    /* Drop any active (e.g. low-duty background) scan first. ble_start_scan_internal()
     * no-ops when a scan is already running, so without this the aggressive discovery
     * params would never take effect once a background scan exists — OP_SHOW would then
     * scan at low duty over a freshly-wiped snapshot and report few/no devices.
     * BLE_HS_EALREADY (nothing to cancel) is fine and ignored. */
    ble_gap_disc_cancel();
    ble_start_scan_internal(0, true);
}

/**
 * Start a gentle time-limited scan after disconnect so the P4's reconnect_task
 * can find re-advertising tags without starving existing connections.
 */
void ble_tether_start_scan_timed(uint32_t duration_ms)
{
    ble_start_scan_internal((uint16_t)(duration_ms / 10), false);
}

/**
 * Cancel an in-flight GAP discovery. No-op if no scan is currently active (BLE_HS_EALREADY
 * is the expected return in that case and is silently ignored).
 */
void ble_tether_stop_scan(void)
{
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel rc=%d", rc);
    }
    /* Restore the permanent low-duty background scan so accel-tag
     * advertisements keep arriving after the aggressive window ends. */
    ble_start_scan_internal(0, false);
}

/**
 * Connects to the advertiser at the given address.
 *
 * @param peer_addr             The advertiser's address (type + 6-byte MAC).
 * @return                      0 on success, non-zero on failure.
 */
static int ble_central_connect(const ble_addr_t *peer_addr)
{
    ble_addr_t own_addr;
    struct ble_gap_multi_conn_params multi_conn_params;
    struct ble_gap_conn_params uncoded_conn_param;
    struct ble_gap_conn_params coded_conn_param;
    int rc;

    if (s_ble_multi_conn_num >= BLE_PEER_MAX_NUM) {
        return 1;
    }

#if !(MYNEWT_VAL(BLE_HOST_ALLOW_CONNECT_WITH_SCAN))
    /* Scanning must be stopped before a connection can be initiated.
     * BLE_HS_EALREADY just means no scan was active, which is fine. */
    rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Failed to cancel scan; rc=%d\n", rc);
        return rc;
    }
#endif // Allows initiation of connection and scanning simultaneously (supports C6)

    /* We won't connect to the same device. Change our static random address to simulate
     * multi-connection with only one central and one peripheral.
     */
    rc = ble_hs_id_gen_rnd(0, &own_addr);
    assert(rc == 0);
    rc = ble_hs_id_set_rnd(own_addr.val);
    assert(rc == 0);

    /* The connection and scan parameters for uncoded phy (1M & 2M). */
    uncoded_conn_param.scan_itvl = BLE_GAP_SCAN_ITVL_MS(300);
    uncoded_conn_param.scan_window = BLE_GAP_SCAN_WIN_MS(100);
    uncoded_conn_param.itvl_min = BLE_GAP_CONN_ITVL_MS(BLE_PREF_CONN_ITVL_MS);
    uncoded_conn_param.itvl_max = BLE_GAP_CONN_ITVL_MS(BLE_PREF_CONN_ITVL_MS);
    uncoded_conn_param.latency = 0;
    uncoded_conn_param.supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(10000);
    uncoded_conn_param.min_ce_len = 0;
    uncoded_conn_param.max_ce_len =  BLE_GAP_CONN_ITVL_MS(BLE_PREF_CONN_ITVL_MS);

    /* The connection and scan parameters for coded phy (125k & 500k) */
    coded_conn_param.scan_itvl = BLE_GAP_SCAN_ITVL_MS(300);
    coded_conn_param.scan_window = BLE_GAP_SCAN_WIN_MS(200);
    coded_conn_param.itvl_min = BLE_GAP_CONN_ITVL_MS(BLE_PREF_CONN_ITVL_MS);
    coded_conn_param.itvl_max = BLE_GAP_CONN_ITVL_MS(BLE_PREF_CONN_ITVL_MS);
    coded_conn_param.latency = 0;
    coded_conn_param.supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(10000);
    coded_conn_param.min_ce_len = 0;
    coded_conn_param.max_ce_len =  BLE_GAP_CONN_ITVL_MS(BLE_PREF_CONN_ITVL_MS);

    /* The parameters for multi-connect. We expect that this connection has at least
     * BLE_PREF_EVT_LEN_MS every interval to Rx and Tx.
     */
    multi_conn_params.scheduling_len_us = BLE_PREF_EVT_LEN_MS * 1000;
    multi_conn_params.own_addr_type = BLE_OWN_ADDR_RANDOM;
    multi_conn_params.peer_addr = (ble_addr_t *)peer_addr;
    multi_conn_params.duration_ms = 8000;
    multi_conn_params.phy_mask = BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK |
                                 BLE_GAP_LE_PHY_CODED_MASK;
    multi_conn_params.phy_1m_conn_params = &uncoded_conn_param;
    multi_conn_params.phy_2m_conn_params = &uncoded_conn_param;
    multi_conn_params.phy_coded_conn_params = &coded_conn_param;

    rc = ble_gap_multi_connect(&multi_conn_params, ble_central_client_gap_event, NULL);

    if (rc) {
        ESP_LOGE(TAG, "Error: Failed to connect to device; addr_type=%d addr=%s; rc=%d\n",
                    peer_addr->type, addr_str(peer_addr->val), rc);
        /* Synchronous failure: no GAP CONNECT event will fire, so nothing else
         * will revive the scan we cancelled above. Resume it here, otherwise a
         * failed pair attempt leaves the scanner dead until reboot (OP_SHOW then
         * returns 0 devices once the snapshot ages out). */
        ble_start_scan_internal(0, false);
    }
    else {
        ESP_LOGI(TAG, "Create connection. -> peer addr %s",  addr_str(peer_addr->val));
    }
    return rc;
}

int ble_tether_connect_addr(const ble_addr_t *peer_addr)
{
    if (peer_addr == NULL) {
        return 1;
    }
    return ble_central_connect(peer_addr);
}

int ble_tether_conn_count(void)
{
    return (int)s_ble_multi_conn_num;
}


static void blecent_on_reset(int reason)
{
    ESP_LOGE(TAG, "Resetting state; reason=%d\n", reason);
}

/**
 * Stack-sync callback invoked once the controller is ready. Configures the multi-connect
 * scheduling common factor, ensures an identity address is set, then starts advertising and
 * scanning so this device can act as both central and peripheral.
 */
static void blecent_on_sync(void)
{
    int rc;

    /*
     * To improve both throughput and stability, it is recommended to set the connection interval
     * as an integer multiple of the `MINIMUM_CONN_INTERVAL`. This `MINIMUM_CONN_INTERVAL` should
     * be calculated based on the total number of connections and the Transmitter/Receiver phy.
     *
     * Note that the `MINIMUM_CONN_INTERVAL` value should meet the condition that:
     *      MINIMUM_CONN_INTERVAL > ((MAX_TIME_OF_PDU * 2) + 150us) * CONN_NUM.
     *
     * For example, if we have 10 connections, maxmum TX/RX length is 251 and the phy is 1M, then
     * the `MINIMUM_CONN_INTERVAL` should be greater than ((261 * 8us) * 2 + 150us) * 10 = 43260us.
     *
     */
    rc = ble_gap_common_factor_set(true, (BLE_PREF_CONN_ITVL_MS * 1000) / 625);
    assert(rc == 0);

    /* Make sure we have proper identity address set (public preferred) */
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* We will function as both the central and peripheral device, connecting to all peripherals
     * with the name of BLE_PEER_NAME. Meanwhile, a connectable advertising will be enabled.
     * In this example, we register two gap callback functions.
     *  - ble_central_client_gap_event: Used by the central.
     *  - ble_central_server_gap_event: Used by the peripheral.
     */
    ble_central_advertise();

    /* Start the low-duty background scan immediately so the adv snapshot is warm
     * before the first OP_SHOW and stays refreshed across connect/disconnect. */
    ble_start_scan_internal(0, false);
}

/**
 * FreeRTOS task that runs the NimBLE host event loop. Spawned by ble_tether_init() and remains
 * resident for the lifetime of the BLE stack.
 *
 * @param param                 Unused; required by the FreeRTOS task signature.
 */
static void blecent_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/**
 * Initializes the NimBLE host stack, registers BLE callbacks, configures GATT services, and
 * spawns the host task. Call once during system startup before any other BLE operation.
 */
void ble_tether_init(void)
{
    int rc;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nimble %d", ret);
        return;
    }

    ble_pair_reset_snapshot(); /* creates s_adv.mtx so callers don't race a NULL mutex */

    ble_hs_cfg.reset_cb = blecent_on_reset;
    ble_hs_cfg.sync_cb  = blecent_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    rc = peer_init(BLE_PEER_MAX_NUM, BLE_PEER_MAX_NUM, BLE_PEER_MAX_NUM, BLE_PEER_MAX_NUM, BLE_PEER_MAX_NUM);
#else
    rc = peer_init(BLE_PEER_MAX_NUM, BLE_PEER_MAX_NUM, BLE_PEER_MAX_NUM, BLE_PEER_MAX_NUM);
#endif
    assert(rc == 0);

#if MYNEWT_VAL(BLE_GATTS)
    ESP_LOGI(TAG, "Central and Peripheral");
    rc = tether_gatt_svr_init();
    assert(rc == 0);
    rc = ble_svc_gap_device_name_set("Tether");
    assert(rc == 0);
    rc = ble_svc_gap_device_appearance_set(0x03C1); /* HID Keyboard */
    assert(rc == 0);
#endif

    ble_store_config_init();

    xTaskCreate(ble_collectdevice_task, "display_rssi", BLE_DISPLAY_RSSI_TASK_STACK, NULL, BLE_DISPLAY_RSSI_TASK_PRIO, NULL);
    nimble_port_freertos_init(blecent_host_task);
}
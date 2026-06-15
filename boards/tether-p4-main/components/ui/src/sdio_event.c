#include "sdio_event.h"
#include "tether_ble.h"
#include "tether_sdio_proto.h"
#include "screens/home.h"
#include "screens/tag_pair.h"
#include "store.h"
#include "models.h"
#include "schedule.h"
#include "clock.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vl53l1_platform.h"
#include "tether_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define LOG_CONNECTED_EVERY_N      16  /* throttle the connected log to ~1.6 s; the
                                          list itself is refreshed every poll */
#define PRESENT_CONFIRM_COUNT      1   /* C6 MOVING_CONFIRM_COUNT already debounces; no need to stack */

static const char *TAG = "sdio_event";

#define DETECT_POLL_INTERVAL_MS   100
#define DETECT_POLL_MAX_BACKOFF_MS 2000   /* max delay after consecutive SDIO failures */
#define DETECT_POLL_TIMEOUT_MS    800
#define CONNECTED_TIMEOUT_MS      800
#define DETECT_POLL_STACK        4096
#define DETECT_POLL_PRIO         5
#define DETECT_POLL_STARTUP_MS   5000     /* let C6 finish auto-reconnect before polling */

#define TAG_GRACE_PERIOD_MS      1500LL
#define RECONNECT_INTERVAL_MS   60000LL
#define RECONNECT_SCAN_TIMEOUT  8000
#define RECONNECT_PAIR_TIMEOUT  5000
#define RECONNECT_STACK         4096

static int64_t s_last_seen_ms[MAX_TAGS];
static bool    s_ever_seen[MAX_TAGS];
static uint8_t s_confirm_streak[MAX_TAGS]; /* consecutive polls where in_scan && connected */
static TaskHandle_t s_detect_poll_task_handle;
static volatile bool s_poll_paused = false; /* gates the detect-poll loop; see sdio_event.h */

static volatile bool  s_reconnect_running  = false;
static          int64_t s_last_reconnect_ms = -RECONNECT_INTERVAL_MS; /* fire immediately on first boot */

/* MACs the user deliberately disconnected. Auto-reconnect skips these until a
 * reboot (RAM-only) or an explicit re-pair clears them. */
static uint8_t s_suppress[MAX_TAGS][6];
static bool    s_suppress_used[MAX_TAGS];

static bool suppress_contains(const uint8_t *mac)
{
    for (int i = 0; i < MAX_TAGS; i++)
        if (s_suppress_used[i] && memcmp(s_suppress[i], mac, 6) == 0) return true;
    return false;
}

static void suppress_add(const uint8_t *mac)
{
    if (suppress_contains(mac)) return;
    for (int i = 0; i < MAX_TAGS; i++)
        if (!s_suppress_used[i]) {
            memcpy(s_suppress[i], mac, 6);
            s_suppress_used[i] = true;
            return;
        }
}

static void suppress_remove(const uint8_t *mac)
{
    for (int i = 0; i < MAX_TAGS; i++)
        if (s_suppress_used[i] && memcmp(s_suppress[i], mac, 6) == 0) {
            s_suppress_used[i] = false;
            return;
        }
}

static inline int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000LL);
}

#define TOF_FAIL_GRACE_MS 4000

static bool tof_user_present(void)
{
    static bool     s_last      = false;
    static uint32_t s_fail_ms   = 0;
    uint16_t dist;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (get_tof_distance(&dist) && dist != UINT16_MAX) {
        s_last    = (dist < TOF_THRESHOLD_MM);
        s_fail_ms = 0;
        return s_last;
    }
    if (s_fail_ms == 0) s_fail_ms = now;
    if ((now - s_fail_ms) < TOF_FAIL_GRACE_MS) return s_last;
    return false;
}

static void process_scan_report(const tether_scan_report_t *report,
                                const tether_conn_t *conns, uint8_t n_conn)
{
    Store   *st  = store_get();
    int64_t  now = now_ms();

    Tag  *missing[MAX_TAGS];
    int   missing_count = 0;
    int   present_count = 0;
    int   total_paired  = 0;

    for (int i = 0; i < MAX_TAGS; i++) {
        Tag *tag = st->tag_id_map[i];
        if (!tag) continue;
        total_paired++;

        /* A tag is present only if it appears in the scan report AND is connected. */
        bool in_scan = false;
        for (int j = 0; j < report->count; j++) {
            if (memcmp(tag->mac_address, report->entries[j], 6) == 0) {
                in_scan = true;
                break;
            }
        }

        bool connected = false;
        for (uint8_t j = 0; j < n_conn; j++) {
            if (memcmp(tag->mac_address, conns[j].mac, 6) == 0) {
                connected = true;
                break;
            }
        }

        bool detected = in_scan && connected;

        if (detected) {
            s_confirm_streak[i]++;
        } else {
            s_confirm_streak[i] = 0;
        }

        bool confirmed = (s_confirm_streak[i] >= PRESENT_CONFIRM_COUNT);

        bool present = confirmed;

        if (confirmed) {
            s_last_seen_ms[i] = now;
            s_ever_seen[i]    = true;
        } else if (s_ever_seen[i] && (now - s_last_seen_ms[i]) < TAG_GRACE_PERIOD_MS) {
            present = true;
        }

        if (present) present_count++;
        else         missing[missing_count++] = tag;

        /* Diagnostic: a tag counted present while it isn't in the live scan tells
         * us which signal is holding it up — connected (C6 link not yet timed out)
         * or grace (recently seen). */
        if (present && !in_scan) {
            ESP_LOGW(TAG, "'%s' present but NOT in scan: connected=%d streak=%u grace_left=%lldms",
                     tag->tag_name, (int)connected, s_confirm_streak[i],
                     (long long)(TAG_GRACE_PERIOD_MS - (now - s_last_seen_ms[i])));
        }
    }

    /* ── Per-user presence gate ────────────────────────────────────────────
     * Only report tags as "missing" when their owner has at least one
     * tag currently detected. If a user is away (none of their tags seen),
     * none of their tags count as missing — they're just absent entirely.
     * ───────────────────────────────────────────────────────────────────── */
    bool user_has_present[MAX_USERS] = {0};
    for (int i = 0; i < MAX_TAGS; i++) {
        Tag *tag = st->tag_id_map[i];
        if (!tag) continue;
        /* Check if this tag is present (not in the missing list) */
        bool is_missing = false;
        for (int m = 0; m < missing_count; m++) {
            if (missing[m] == tag) { is_missing = true; break; }
        }
        if (is_missing) continue;
        /* Tag is present — mark its owner as present */
        for (int u = 0; u < st->user_count; u++) {
            if (st->users[u].user_id == tag->user_id) {
                user_has_present[u] = true;
                break;
            }
        }
    }

    /* Filter missing list: only keep tags whose owner is present */
    int filtered_count = 0;
    for (int m = 0; m < missing_count; m++) {
        bool owner_present = false;
        for (int u = 0; u < st->user_count; u++) {
            if (st->users[u].user_id == missing[m]->user_id && user_has_present[u]) {
                owner_present = true;
                break;
            }
        }
        if (owner_present) {
            missing[filtered_count++] = missing[m];
        }
    }
    missing_count = filtered_count;

    bool user_present = tof_user_present();

    /* ── Schedule gate ──────────────────────────────────────────────────────
     * If the store has schedules configured, check whether any are active
     * right now. If none are active, stay UNKNOWN (silent / no alert).
     * If no schedules are configured at all, treat as always-on.
     * ─────────────────────────────────────────────────────────────────────── */
    bool schedule_active = true; /* default: always-on */
    if (st->schedule_count > 0) {
        ClockNow clk;
        if (clock_get_now(&clk) == ESP_OK) {
            Schedule *active_scheds[MAX_SCHEDULES];
            int active_count = 0;
            store_schedule_get_active(clk.hour, clk.minute, clk.day,
                                      active_scheds, MAX_SCHEDULES, &active_count);
            schedule_active = (active_count > 0);
        }
    }

    tether_status_t status;
    if (total_paired == 0) {
        status = TETHER_STATUS_UNKNOWN;
    } else if (!user_present) {
        status = TETHER_STATUS_UNKNOWN;
    } else if (!schedule_active) {
        status = TETHER_STATUS_UNKNOWN;
    } else if (present_count == 0) {
        /* A person is at the threshold (TOF) but not a single paired tag is
         * detected. The per-user gate above zeroed out missing_count because
         * no owner has any present tag — but that's exactly the "left without
         * any tag" case, which must not read as OK. */
        status = TETHER_STATUS_OK;
    } else if (missing_count == 0) {
        status = TETHER_STATUS_OK;
    } else {
        status = TETHER_STATUS_MISSING;
    }

    ESP_LOGI(TAG, "scan: %d paired, %u connected, %d present, %d missing, tof=%d -> %s",
             total_paired, n_conn, present_count, missing_count,
             (int)user_present,
             status == TETHER_STATUS_OK ? "OK" :
             status == TETHER_STATUS_MISSING ? "MISSING" : "UNKNOWN");

    bsp_display_lock(0);
    home_set_status(status);
    if (status == TETHER_STATUS_MISSING) {
        home_set_missing(missing, missing_count);
    }
    bsp_display_unlock();
}

static void on_ble_event(const tether_ble_event_t *evt, void *user)
{
    (void)user;
    switch (evt->kind) {
        case TETHER_BLE_EVT_PAIR_COMPLETE: {
            /* A successful (re)pair means the user wants it back — un-suppress. */
            if (evt->pair_complete.success) {
                suppress_remove(evt->pair_complete.mac);
            }

            if (evt->pair_complete.connection_type == TETHER_CENT_TO_PERIPHERAL) {
                tag_pair_on_pair_complete(evt->pair_complete.mac,
                                        evt->pair_complete.success);
                break;
            }

            /* ===== PERIPHERAL->CENTRAL =====*/
            /* connection_type == TETHER_PERIPHERAL_TO_CENT */
            if (!evt->pair_complete.success) {
                break;
            }

            /* Peripheral-initiated pair: the tag connected to us, so no pairing
             * wizard ran and we have no name/user/schedule for it. */
            Tag *existing = NULL;
            if (store_tag_find_by_mac(evt->pair_complete.mac, &existing) == ESP_OK
                && existing != NULL) {
                /* Already a known tag — just a reconnect, nothing to add. */
                ESP_LOGI(TAG, "peripheral reconnected: '%s'", existing->tag_name);
            } else {
                /* Genuinely new device — needs the user to name/assign it. */
                ESP_LOGI(TAG, "peripheral pair from unknown device — awaiting UI prompt");
                // hand off to my beautiful lvgl 
                tag_pair_on_incoming_pair(evt->pair_complete.mac);
            }
            break;
        }

        case TETHER_BLE_EVT_PEER_DISCONN:
            /* intentional flag is set by the C6 (ground truth: it dropped the peer
             * for an OP_DISCONNECT/OP_REMOVE). */
            if (evt->peer_disconn.intentional) {
                /* If still in the store this was a `disconnect` (a `remove` deletes
                 * first) — suppress reconnect until reboot/re-pair. */
                Tag *tag = NULL;
                if (store_tag_find_by_mac(evt->peer_disconn.mac, &tag) == ESP_OK && tag) {
                    suppress_add(evt->peer_disconn.mac);
                    ESP_LOGI(TAG, "peer disconnect (0x%04x) — intentional, suppressing reconnect",
                             evt->peer_disconn.reason);
                }
            } else {
                /* Unintentional drop — clear the rate-limit so the next poll
                 * reconnects now. Still store-gated, so removed tags stay gone. */
                ESP_LOGI(TAG, "peer disconnect (0x%04x) — unintentional, reconnecting",
                         evt->peer_disconn.reason);
                s_last_reconnect_ms = now_ms() - RECONNECT_INTERVAL_MS;
            }
            break;

        case TETHER_BLE_EVT_PAIR_REMOVE: {
            suppress_remove(evt->pair_remove.mac);
            Tag *tag = NULL;
            if (store_tag_find_by_mac(evt->pair_remove.mac, &tag) == ESP_OK && tag != NULL) {
                store_tag_delete(tag->tag_id);
            }
            break;
        }
    }
}

static void log_connected(const tether_conn_t *conns, uint8_t n)
{
    if (n == 0) {
        ESP_LOGI(TAG, "connected: none");
        return;
    }
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *m = conns[i].mac;
        ESP_LOGI(TAG, "connected[%u]: %02X:%02X:%02X:%02X:%02X:%02X  %d dBm",
                 i, m[5], m[4], m[3], m[2], m[1], m[0], conns[i].rssi);
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;

    tether_dev_t devs[8];
    uint8_t      count = 0;
    if (tether_ble_show(devs, 8, &count, RECONNECT_SCAN_TIMEOUT) != ESP_OK || count == 0) {
        ESP_LOGW(TAG, "auto-reconnect: scan failed or no devices found");
        s_reconnect_running = false;
        vTaskDelete(NULL);
        return;
    }

    tether_conn_t conns[16];
    uint8_t n_conn = 0;
    tether_ble_connected(conns, 16, &n_conn, CONNECTED_TIMEOUT_MS);

    for (uint8_t i = 0; i < count; i++) {
        Tag *tag = NULL;
        if (store_tag_find_by_mac(devs[i].mac, &tag) != ESP_OK || tag == NULL) continue;
        if (suppress_contains(devs[i].mac)) continue;  /* user disconnected it on purpose */

        bool already_connected = false;
        for (uint8_t j = 0; j < n_conn; j++) {
            if (memcmp(conns[j].mac, devs[i].mac, 6) == 0) {
                already_connected = true;
                break;
            }
        }
        if (already_connected) continue;

        ESP_LOGI(TAG, "auto-reconnect: pairing '%s'", tag->tag_name);
        tether_ble_pair_by_mac(devs[i].mac, RECONNECT_PAIR_TIMEOUT);
    }

    s_reconnect_running = false;
    vTaskDelete(NULL);
}

/* True if any paired, non-suppressed tag is not currently connected. */
static bool reconnect_needed(const tether_conn_t *conns, uint8_t n_conn)
{
    Store *st = store_get();
    for (int i = 0; i < MAX_TAGS; i++) {
        Tag *tag = st->tag_id_map[i];
        if (!tag) continue;
        if (suppress_contains(tag->mac_address)) continue;
        bool connected = false;
        for (uint8_t j = 0; j < n_conn; j++)
            if (memcmp(conns[j].mac, tag->mac_address, 6) == 0) { connected = true; break; }
        if (!connected) return true;
    }
    return false;
}

static void maybe_reconnect(const tether_conn_t *conns, uint8_t n_conn)
{
    if (s_reconnect_running) return;
    if (!reconnect_needed(conns, n_conn)) return;

    int64_t now = now_ms();
    if (now - s_last_reconnect_ms < RECONNECT_INTERVAL_MS) return;

    s_last_reconnect_ms = now;
    s_reconnect_running = true;

    ESP_LOGI(TAG, "auto-reconnect: scanning");

    BaseType_t ok = xTaskCreate(reconnect_task, "reconnect", RECONNECT_STACK, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "auto-reconnect: task create failed");
        s_reconnect_running = false;
    }
}

void sdio_event_pause_polling(void)
{
    s_poll_paused = true;
    ESP_LOGI(TAG, "detect poll paused");
}

void sdio_event_resume_polling(void)
{
    s_poll_paused = false;
    ESP_LOGI(TAG, "detect poll resumed");
}

static void detect_poll_task(void *arg)
{
    (void)arg;
    int s_log_counter = 0;

    /* Let the C6 finish auto-reconnecting before we start hammering SDIO.
     * NimBLE's host task runs at prio 21 during connection setup and starves
     * the SDIO slave task (prio 10). */
    vTaskDelay(pdMS_TO_TICKS(DETECT_POLL_STARTUP_MS));

    /* Live connected list — refreshed every poll so presence reflects reality.
     * Prime it immediately so the first poll has a valid value. */
    static tether_conn_t s_conns[16];
    static uint8_t       s_n_conn = 0;
    tether_ble_connected(s_conns, 16, &s_n_conn, CONNECTED_TIMEOUT_MS);

    int poll_delay_ms = DETECT_POLL_INTERVAL_MS;
    int fail_log_skip = 0;  /* throttle failure warnings */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(poll_delay_ms));

        /* While paused (e.g. pairing screen open) leave the C6 radio alone so a
         * discovery scan can run. Reset the rate to normal for when we resume. */
        if (s_poll_paused) {
            poll_delay_ms = DETECT_POLL_INTERVAL_MS;
            continue;
        }

        /* Refresh the connected snapshot every poll. On failure keep the last
         * value rather than falsely reporting zero connections. */
        if (tether_ble_connected(s_conns, 16, &s_n_conn, CONNECTED_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGW(TAG, "connected query failed");
        }

        if (++s_log_counter >= LOG_CONNECTED_EVERY_N) {
            log_connected(s_conns, s_n_conn);
            s_log_counter = 0;
        }

        tether_scan_report_t report = {0};
        esp_err_t err = tether_ble_detected(&report, DETECT_POLL_TIMEOUT_MS);
        if (err != ESP_OK) {
            /* Exponential backoff: double the delay on each failure, cap at max.
             * This stops us from flooding the SDIO bus when the C6 is busy
             * (e.g. during auto-reconnect at boot). */
            if (poll_delay_ms < DETECT_POLL_MAX_BACKOFF_MS)
                poll_delay_ms = poll_delay_ms * 2;
            if (poll_delay_ms > DETECT_POLL_MAX_BACKOFF_MS)
                poll_delay_ms = DETECT_POLL_MAX_BACKOFF_MS;

            /* Only log every 4th failure to avoid spamming */
            if (++fail_log_skip >= 4) {
                ESP_LOGW(TAG, "detect poll failed: %s (backoff %dms)",
                         esp_err_to_name(err), poll_delay_ms);
                fail_log_skip = 0;
            }
            continue;
        }

        /* Success — reset to normal polling rate */
        poll_delay_ms = DETECT_POLL_INTERVAL_MS;
        fail_log_skip = 0;

        process_scan_report(&report, s_conns, s_n_conn);

        /* auto-reconnect: if any paired, non-suppressed tag isn't connected. */
        maybe_reconnect(s_conns, s_n_conn);
    }
}

void sdio_event_start(essl_handle_t handle)
{
    (void)handle;
    tether_ble_set_event_cb(on_ble_event, NULL);
    ESP_LOGI(TAG, "SDIO event callback registered");

    if (s_detect_poll_task_handle == NULL) {
        BaseType_t ok = xTaskCreate(detect_poll_task, "detect_poll",
                                    DETECT_POLL_STACK, NULL,
                                    DETECT_POLL_PRIO,
                                    &s_detect_poll_task_handle);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "detect poll task create failed");
            s_detect_poll_task_handle = NULL;
        }
    }
}

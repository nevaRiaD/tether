#include "tag_pair.h"
#include "screen_manager.h"
#include "theme.h"
#include "store.h"
#include "user_mgmt.h"
#include "tether_ble.h"
#include "sdio_event.h"
#include "tether_config.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tag_pair";

/* ── State machine ─────────────────────────────────────────────────── */

typedef enum {
    STATE_SCANNING,
    STATE_DETECTED,
    STATE_PAIRING,
    STATE_NAMING,
    STATE_NO_DEVICES,
} PairState;

/* ── Screen-lifetime state ─────────────────────────────────────────── */

static volatile bool   s_screen_active = false;

static PairState        s_state         = STATE_SCANNING;
static lv_obj_t        *s_state_panel   = NULL;
static lv_obj_t        *s_name_ta       = NULL;
static lv_obj_t        *s_keyboard      = NULL;

/* Scan results */
static tether_dev_t     s_devs[20];
static uint8_t          s_dev_count     = 0;
static tether_dev_t     s_best_dev;
static bool             s_have_dev      = false;

/* Name typed by the user (copied before bg task runs) */
static char             s_pending_name[32];
static bool             s_reconnecting  = false;
static bool             s_incoming  = false;

/* User & schedule selection during pairing */
static uint32_t         s_pair_user_id     = 0;
static uint32_t         s_pair_schedule_id = UINT32_MAX;  /* UINT32_MAX = always-on */

/* ── Helpers ───────────────────────────────────────────────────────── */

static void anim_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void pulse(lv_obj_t *target, uint32_t half_ms) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_time(&a, half_ms);
    lv_anim_set_playback_time(&a, half_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static lv_obj_t *make_ring(lv_obj_t *parent, int d, lv_color_t color, lv_opa_t opa) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, d, d);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_0, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, color, 0);
    lv_obj_set_style_border_opa(r, opa, 0);
    lv_obj_align(r, LV_ALIGN_CENTER, 0, -30);
    return r;
}

/* ── Forward declarations ──────────────────────────────────────────── */
static void save_cb(lv_event_t *e);
static void scan_task(void *arg);
static void pair_task(void *arg);
static void set_state(PairState new_state);

/* ── State panel builders ──────────────────────────────────────────── */

static lv_obj_t *build_scanning(lv_obj_t *parent) {
    const TetherTheme *t = theme_get();

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

    make_ring(panel, 320, t->gold, LV_OPA_10);
    make_ring(panel, 240, t->ring, LV_OPA_30);
    make_ring(panel, 160, t->ring, LV_OPA_40);
    make_ring(panel,  90, t->scan, LV_OPA_30);

    lv_obj_t *dot = lv_obj_create(panel);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, 60, 60);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, t->scan, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -30);
    pulse(dot, 900);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, "Scanning for tags...");
    lv_obj_set_style_text_color(label, t->text, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *sub = lv_label_create(panel);
    lv_label_set_text(sub, "make sure your tag is nearby");
    lv_obj_set_style_text_color(sub, t->muted, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 90);

    return panel;
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    if (!s_screen_active) return;
    set_state(STATE_SCANNING);
    xTaskCreate(scan_task, "scan", 4096, NULL, 4, NULL);
}

static lv_obj_t *build_no_devices(lv_obj_t *parent) {
    const TetherTheme *t = theme_get();

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, "No tags found");
    lv_obj_set_style_text_color(label, t->text, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *sub = lv_label_create(panel);
    lv_label_set_text(sub, "make sure your tag is on and nearby");
    lv_obj_set_style_text_color(sub, t->muted, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *btn = lv_btn_create(panel);
    lv_obj_set_size(btn, 200, 48);
    lv_obj_set_style_bg_color(btn, t->gold, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_event_cb(btn, rescan_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Scan again");
    lv_obj_set_style_text_color(btn_lbl, t->bg, 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(btn_lbl);

    return panel;
}

static void dev_row_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_dev_count) return;
    s_best_dev = s_devs[idx];
    s_have_dev = true;

    Tag *existing = NULL;
    if (store_tag_find_by_mac(s_best_dev.mac, &existing) == ESP_OK && existing != NULL) {
        snprintf(s_pending_name, sizeof(s_pending_name), "%s", existing->tag_name);
        s_reconnecting = true;
        xTaskCreate(pair_task, "pair", 4096, NULL, 5, NULL);
        set_state(STATE_PAIRING);
    } else {
        s_reconnecting = false;
        set_state(STATE_NAMING);
    }
}

static lv_obj_t *build_detected(lv_obj_t *parent) {
    const TetherTheme *t = theme_get();

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

    /* Sub-header */
    lv_obj_t *sub = lv_label_create(panel);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d device%s found — tap to pair",
             s_dev_count, s_dev_count == 1 ? "" : "s");
    lv_label_set_text(sub, buf);
    lv_obj_set_style_text_color(sub, t->muted, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 16);

    /* Scrollable list */
    lv_obj_t *list = lv_obj_create(panel);
    lv_obj_set_size(list, 700, 440);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_opa(list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (uint8_t i = 0; i < s_dev_count; i++) {
        const tether_dev_t *d = &s_devs[i];

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), 64);
        lv_obj_set_style_bg_color(row, t->row_bg, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 16, 0);
        lv_obj_set_style_pad_right(row, 16, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, dev_row_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        /* Name or MAC on the left */
        lv_obj_t *name_lbl = lv_label_create(row);
        if (strlen(d->name) > 0) {
            lv_label_set_text(name_lbl, d->name);
        } else {
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     d->mac[0], d->mac[1], d->mac[2],
                     d->mac[3], d->mac[4], d->mac[5]);
            lv_label_set_text(name_lbl, mac_str);
        }
        lv_obj_set_style_text_color(name_lbl, t->text, 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, -8);

        /* MAC sub-label (only when name is shown) */
        if (strlen(d->name) > 0) {
            lv_obj_t *mac_lbl = lv_label_create(row);
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     d->mac[0], d->mac[1], d->mac[2],
                     d->mac[3], d->mac[4], d->mac[5]);
            lv_label_set_text(mac_lbl, mac_str);
            lv_obj_set_style_text_color(mac_lbl, t->muted, 0);
            lv_obj_set_style_text_font(mac_lbl, &lv_font_montserrat_12, 0);
            lv_obj_align(mac_lbl, LV_ALIGN_LEFT_MID, 0, 10);
        }

        /* Reconnect badge or RSSI on the right */
        Tag *existing_tag = NULL;
        bool is_known = (store_tag_find_by_mac(d->mac, &existing_tag) == ESP_OK && existing_tag != NULL);
        if (is_known) {
            lv_obj_t *badge = lv_label_create(row);
            lv_label_set_text(badge, "Reconnect");
            lv_obj_set_style_text_color(badge, t->gold, 0);
            lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
            lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
        } else {
            lv_obj_t *rssi_lbl = lv_label_create(row);
            char rssi_str[12];
            snprintf(rssi_str, sizeof(rssi_str), "%d dBm", d->rssi);
            lv_label_set_text(rssi_lbl, rssi_str);
            lv_obj_set_style_text_color(rssi_lbl, t->muted, 0);
            lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_12, 0);
            lv_obj_align(rssi_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    return panel;
}

static lv_obj_t *build_pairing(lv_obj_t *parent) {
    const TetherTheme *t = theme_get();

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *dot = lv_obj_create(panel);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, 60, 60);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, t->gold, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -30);
    pulse(dot, 400);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, "Connecting to tag...");
    lv_obj_set_style_text_color(label, t->text, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *sub = lv_label_create(panel);
    lv_label_set_text(sub, "keep the tag nearby");
    lv_obj_set_style_text_color(sub, t->muted, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 90);

    return panel;
}

static void kb_ready_cb(lv_event_t *e) {
    (void)e;
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void ta_focused_cb(lv_event_t *e) {
    (void)e;
    if (s_keyboard) {
        lv_keyboard_set_textarea(s_keyboard, s_name_ta);
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pair_user_dd_cb(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    Store *st = store_get();
    if ((int)sel < st->user_count)
        s_pair_user_id = st->users[sel].user_id;
}

static void pair_schedule_dd_cb(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel == 0) {
        s_pair_schedule_id = UINT32_MAX;
    } else {
        Store *st = store_get();
        int idx = 0;
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            if (!st->schedule_id_map[i]) continue;
            if (idx == (int)(sel - 1)) {
                s_pair_schedule_id = st->schedule_id_map[i]->schedule_id;
                break;
            }
            idx++;
        }
    }
}

static lv_obj_t *build_naming(lv_obj_t *parent) {
    const TetherTheme *t = theme_get();

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);

    /* Initialize defaults */
    s_pair_user_id     = user_mgmt_get_active_user_id();
    s_pair_schedule_id = UINT32_MAX;

    Store *st = store_get();

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, "Name your tag");
    lv_obj_set_style_text_color(label, t->text, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -200);

    s_name_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_name_ta, 400, 48);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_placeholder_text(s_name_ta, "e.g. Keys");
    lv_obj_set_style_bg_color(s_name_ta, t->row_bg, 0);
    lv_obj_set_style_text_color(s_name_ta, t->text, 0);
    lv_obj_set_style_text_color(s_name_ta, t->muted, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(s_name_ta, t->line, 0);
    lv_obj_set_style_border_width(s_name_ta, 1, 0);
    lv_obj_set_style_radius(s_name_ta, 8, 0);
    lv_obj_align(s_name_ta, LV_ALIGN_CENTER, 0, -145);
    lv_obj_add_event_cb(s_name_ta, ta_focused_cb, LV_EVENT_FOCUSED, NULL);

    /* ── User dropdown ── */
    if (st->user_count > 0) {
        lv_obj_t *user_lbl = lv_label_create(panel);
        lv_label_set_text(user_lbl, "User");
        lv_obj_set_style_text_color(user_lbl, t->muted, 0);
        lv_obj_set_style_text_font(user_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(user_lbl, LV_ALIGN_CENTER, -110, -105);

        char user_opts[256];
        int ulen = 0;
        uint16_t user_sel = 0;
        for (int i = 0; i < st->user_count; i++) {
            if (i > 0) user_opts[ulen++] = '\n';
            ulen += snprintf(user_opts + ulen, sizeof(user_opts) - ulen,
                             "%s", st->users[i].username);
            if (st->users[i].user_id == s_pair_user_id)
                user_sel = (uint16_t)i;
        }

        lv_obj_t *user_dd = lv_dropdown_create(panel);
        lv_obj_set_size(user_dd, 190, 40);
        lv_dropdown_set_options(user_dd, user_opts);
        lv_dropdown_set_selected(user_dd, user_sel);
        lv_obj_align(user_dd, LV_ALIGN_CENTER, -105, -70);

        lv_obj_set_style_bg_color(user_dd, t->row_bg, 0);
        lv_obj_set_style_border_color(user_dd, t->line, 0);
        lv_obj_set_style_border_width(user_dd, 1, 0);
        lv_obj_set_style_text_color(user_dd, t->text, 0);
        lv_obj_set_style_text_font(user_dd, &lv_font_montserrat_14, 0);

        lv_obj_t *udd_list = lv_dropdown_get_list(user_dd);
        lv_obj_set_style_bg_color(udd_list, t->row_bg, 0);
        lv_obj_set_style_border_color(udd_list, t->line, 0);
        lv_obj_set_style_text_color(udd_list, t->text, 0);
        lv_obj_set_style_text_font(udd_list, &lv_font_montserrat_14, 0);
        lv_obj_set_style_bg_color(udd_list, t->scan,
                                   LV_PART_SELECTED | LV_STATE_CHECKED);

        lv_obj_add_event_cb(user_dd, pair_user_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* ── Schedule dropdown ── */
    {
        lv_obj_t *sched_lbl = lv_label_create(panel);
        lv_label_set_text(sched_lbl, "Schedule");
        lv_obj_set_style_text_color(sched_lbl, t->muted, 0);
        lv_obj_set_style_text_font(sched_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(sched_lbl, LV_ALIGN_CENTER, 105, -105);

        char sched_opts[512];
        int slen = 0;
        slen += snprintf(sched_opts + slen, sizeof(sched_opts) - slen, "Always On");
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            Schedule *sc = st->schedule_id_map[i];
            if (!sc) continue;
            slen += snprintf(sched_opts + slen, sizeof(sched_opts) - slen,
                             "\n%s", sc->schedule_name);
        }

        lv_obj_t *sched_dd = lv_dropdown_create(panel);
        lv_obj_set_size(sched_dd, 190, 40);
        lv_dropdown_set_options(sched_dd, sched_opts);
        lv_dropdown_set_selected(sched_dd, 0);
        lv_obj_align(sched_dd, LV_ALIGN_CENTER, 105, -70);

        lv_obj_set_style_bg_color(sched_dd, t->row_bg, 0);
        lv_obj_set_style_border_color(sched_dd, t->line, 0);
        lv_obj_set_style_border_width(sched_dd, 1, 0);
        lv_obj_set_style_text_color(sched_dd, t->text, 0);
        lv_obj_set_style_text_font(sched_dd, &lv_font_montserrat_14, 0);

        lv_obj_t *sdd_list = lv_dropdown_get_list(sched_dd);
        lv_obj_set_style_bg_color(sdd_list, t->row_bg, 0);
        lv_obj_set_style_border_color(sdd_list, t->line, 0);
        lv_obj_set_style_text_color(sdd_list, t->text, 0);
        lv_obj_set_style_text_font(sdd_list, &lv_font_montserrat_14, 0);
        lv_obj_set_style_bg_color(sdd_list, t->scan,
                                   LV_PART_SELECTED | LV_STATE_CHECKED);

        lv_obj_add_event_cb(sched_dd, pair_schedule_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    lv_obj_t *btn = lv_btn_create(panel);
    lv_obj_set_size(btn, 200, 48);
    lv_obj_set_style_bg_color(btn, t->gold, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -5);
    lv_obj_add_event_cb(btn, save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Save & Pair");
    lv_obj_set_style_text_color(btn_lbl, t->bg, 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(btn_lbl);

    s_keyboard = lv_keyboard_create(parent);
    lv_obj_set_style_bg_color(s_keyboard, t->row_bg, 0);
    lv_obj_set_style_bg_color(s_keyboard, t->line,   LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard, t->text,  LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_keyboard, t->bg,  LV_PART_ITEMS);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, kb_ready_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, kb_ready_cb, LV_EVENT_CANCEL, NULL);

    return panel;
}

/* ── State transitions ─────────────────────────────────────────────── */

static void set_state(PairState new_state)
{
    s_state = new_state;
    if (s_state_panel) {
        lv_obj_del(s_state_panel);
        s_state_panel = NULL;
    }
    s_name_ta  = NULL;
    s_keyboard = NULL;

    lv_obj_t *scr = lv_scr_act();
    switch (new_state) {
        case STATE_SCANNING: s_state_panel = build_scanning(scr); break;
        case STATE_DETECTED: s_state_panel = build_detected(scr); break;
        case STATE_PAIRING:  s_state_panel = build_pairing(scr);  break;
        case STATE_NAMING:   s_state_panel = build_naming(scr);   break;
        case STATE_NO_DEVICES: s_state_panel = build_no_devices(scr); break;
    }
}

/* ── Button callbacks ──────────────────────────────────────────────── */

/* pair_task: sends TETHER_OP_PAIR to C6 (blocking), then shows PAIRING state */
static void pair_task(void *arg)
{
    (void)arg;
    if (!s_have_dev) { vTaskDelete(NULL); return; }

    esp_err_t err = tether_ble_pair_by_mac(s_best_dev.mac, 5000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tether_ble_pair: %s", esp_err_to_name(err));
        if (s_screen_active) {
            bsp_display_lock(0);
            screen_manager_load(SCREEN_HOME);
            bsp_display_unlock();
        }
        vTaskDelete(NULL);
        return;
    }

    /* C6 ACK'd — now we wait for PAIR_COMPLETE event (tag_pair_on_pair_complete) */
    bsp_display_lock(0);
    if (s_screen_active) set_state(STATE_PAIRING);
    bsp_display_unlock();
    vTaskDelete(NULL);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_dev) return;

    const char *text = s_name_ta ? lv_textarea_get_text(s_name_ta) : "";
    if (!text || strlen(text) == 0) text = "Unnamed Tag";
    snprintf(s_pending_name, sizeof(s_pending_name), "%s", text);

    if (s_incoming) {
        Tag *new_tag = NULL;
        store_tag_add(s_best_dev.mac, s_pending_name, &new_tag, s_pair_user_id);
        if (new_tag && s_pair_schedule_id != UINT32_MAX) {
            store_schedule_add_tag(s_pair_schedule_id, new_tag->tag_id);
            store_save_schedules();
        }
        store_save_tags();
        screen_manager_load(SCREEN_HOME);
        return;
    }

    /* pair_task calls tether_ble_pair (blocks ~1 s) — run off-task */
    xTaskCreate(pair_task, "pair", 4096, NULL, 5, NULL);
}

/* ── Scan background task ──────────────────────────────────────────── */

static void scan_task(void *arg)
{
    (void)arg;

    tether_dev_t devs[20];
    uint8_t      count = 0;

    /* Retry a few times — discovery scans often come back empty on the first
     * pass while the C6 settles. Give up after PAIR_SCAN_MAX_ATTEMPTS and show a
     * terminal "no tags found" state instead of spinning forever. */
    for (int attempt = 1; attempt <= PAIR_SCAN_MAX_ATTEMPTS; attempt++) {
        if (!s_screen_active) { vTaskDelete(NULL); return; }

        ESP_LOGI(TAG, "BLE scan attempt %d/%d…", attempt, PAIR_SCAN_MAX_ATTEMPTS);
        esp_err_t err = tether_ble_show(devs, 20, &count, PAIR_SCAN_TIMEOUT_MS);

        if (err == ESP_OK && count > 0) {
            /* Save full list — user picks from the UI */
            memcpy(s_devs, devs, count * sizeof(tether_dev_t));
            s_dev_count = count;
            s_have_dev  = false;   /* set when user taps a row */

            ESP_LOGI(TAG, "found %d devices:", count);
            for (uint8_t i = 0; i < count; i++) {
                ESP_LOGI(TAG, "  [%d] rssi=%d name='%s'", i, devs[i].rssi, devs[i].name);
            }

            bsp_display_lock(0);
            if (s_screen_active) set_state(STATE_DETECTED);
            bsp_display_unlock();
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG, "scan attempt %d: %s, %d devices found",
                 attempt, esp_err_to_name(err), count);
        vTaskDelay(pdMS_TO_TICKS(PAIR_SCAN_RETRY_DELAY_MS));
    }

    /* Exhausted all attempts — let the UI stop scanning. */
    bsp_display_lock(0);
    if (s_screen_active) set_state(STATE_NO_DEVICES);
    bsp_display_unlock();
    vTaskDelete(NULL);
}

/* ── Called from sdio_event.c when PAIR_COMPLETE event arrives ─────── */

void tag_pair_on_pair_complete(const uint8_t mac[6], bool success)
{
    if (!s_screen_active) return;

    if (!success) {
        ESP_LOGW(TAG, "pair failed — restarting scan");
        s_have_dev = false;
        bsp_display_lock(0);
        if (s_screen_active) set_state(STATE_SCANNING);
        bsp_display_unlock();
        xTaskCreate(scan_task, "scan", 4096, NULL, 4, NULL);
        return;
    }

    if (!s_reconnecting) {
        Tag *new_tag = NULL;
        store_tag_add(mac, s_pending_name, &new_tag, s_pair_user_id);

        /* Assign to selected schedule if one was chosen */
        if (new_tag && s_pair_schedule_id != UINT32_MAX) {
            store_schedule_add_tag(s_pair_schedule_id, new_tag->tag_id);
            store_save_schedules();
        }
        store_save_tags();
    }

    ESP_LOGI(TAG, "tag '%s' saved", s_pending_name);

    bsp_display_lock(0);
    if (s_screen_active) screen_manager_load(SCREEN_HOME);
    bsp_display_unlock();
}

void tag_pair_on_incoming_pair(const uint8_t mac[6])
{
    memcpy(s_best_dev.mac, mac, 6);
    s_best_dev.name[0] = '\0';
    s_have_dev     = true;
    s_incoming     = true;
    s_reconnecting = false;

    bsp_display_lock(0);
    screen_manager_load(SCREEN_TAG_PAIR);
    bsp_display_unlock();
}

/* ── Screen lifecycle ──────────────────────────────────────────────── */

static void back_cb(lv_event_t *e) {
    (void)e;
    screen_manager_load(SCREEN_HOME);
}

static void screen_delete_cb(lv_event_t *e) {
    (void)e;
    s_screen_active = false;
    sdio_event_resume_polling();
    s_state_panel   = NULL;
    s_keyboard      = NULL;
    s_name_ta       = NULL;
    s_state         = STATE_SCANNING;
    s_have_dev         = false;
    s_dev_count        = 0;
    s_reconnecting     = false;
    s_incoming		   = false;
    s_pair_user_id     = 0;
    s_pair_schedule_id = UINT32_MAX;
}

lv_obj_t *tag_pair_screen_create(void)
{
    const TetherTheme *t = theme_get();

    s_screen_active = true;
    if (!s_incoming) s_have_dev = false;

    /* Free the C6 radio for discovery while this screen is up. */
    sdio_event_pause_polling();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, t->bg, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, NULL);

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 34, 34);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 28, 18);
    lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, t->text, 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(back_lbl);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Pair Tag");
    lv_obj_set_style_text_color(title, t->text, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 72, 20);

    /* Divider */
    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, 960, 1);
    lv_obj_set_style_bg_color(rule, t->line, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 60);

	/* discriminate between already bonded vs. not */
    if (s_incoming) {
        s_pending_name[0] = '\0';
        s_state       = STATE_NAMING;
        s_state_panel = build_naming(scr);
    } else {
        s_state       = STATE_SCANNING;
        s_state_panel = build_scanning(scr);
        xTaskCreate(scan_task, "scan", 4096, NULL, 4, NULL);
    }

    return scr;
}

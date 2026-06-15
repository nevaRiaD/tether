#include "tag_edit.h"
#include "screen_manager.h"
#include "theme.h"
#include "store.h"
#include "models.h"
#include "tether_ble.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* delete_cb asks the C6 to erase a removed tag's bond off the LVGL thread — the SDIO
 * transactions block for up to a few seconds. */
#define UNPAIR_CONNECTED_TIMEOUT_MS  1000
#define UNPAIR_REMOVE_TIMEOUT_MS     3000

#define NO_SCHEDULE_LABEL  "Always On"

static uint32_t  s_tag_id;
static char      s_name[32];
static uint32_t  s_selected_schedule_id;   /* UINT32_MAX = none / always-on */
static uint32_t  s_selected_user_id;       /* user this tag is assigned to  */

static lv_obj_t *s_kbd = NULL;

/* ── callbacks ───────────────────────────────────────── */
static void back_cb(lv_event_t *e)  { (void)e; screen_manager_load(SCREEN_USER); }

static void ta_focused_cb(lv_event_t *e)
{
    if (s_kbd) {
        lv_keyboard_set_textarea(s_kbd, lv_event_get_target(e));
        lv_obj_clear_flag(s_kbd, LV_OBJ_FLAG_HIDDEN);
    }
}
static void ta_changed_cb(lv_event_t *e)
{
    const char *txt = lv_textarea_get_text(lv_event_get_target(e));
    strncpy(s_name, txt, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
}
static void kbd_ready_cb(lv_event_t *e)
{
    (void)e;
    if (s_kbd) lv_obj_add_flag(s_kbd, LV_OBJ_FLAG_HIDDEN);
}

static void dropdown_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);

    /* sel 0 = "Always On", sel 1..N = schedule[sel-1] */
    if (sel == 0) {
        s_selected_schedule_id = UINT32_MAX;
    } else {
        Store *st = store_get();
        int idx = 0;
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            if (!st->schedule_id_map[i]) continue;
            if (idx == (int)(sel - 1)) {
                s_selected_schedule_id = st->schedule_id_map[i]->schedule_id;
                break;
            }
            idx++;
        }
    }
}

static void user_dropdown_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    Store *st = store_get();
    if ((int)sel < st->user_count)
        s_selected_user_id = st->users[sel].user_id;
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    Store *st = store_get();
    Tag *tag = NULL;
    if (store_tag_find_by_id(s_tag_id, &tag) != ESP_OK || !tag) {
        screen_manager_load(SCREEN_USER);
        return;
    }

    /* rename + reassign user */
    store_tag_update_settings(s_tag_id, s_name, s_selected_user_id);

    /* remove tag from all schedules first, then assign selected one */
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (st->schedule_id_map[i]) {
            store_schedule_remove_tag(st->schedule_id_map[i]->schedule_id, s_tag_id);
        }
    }
    if (s_selected_schedule_id != UINT32_MAX) {
        store_schedule_add_tag(s_selected_schedule_id, s_tag_id);
    }

    store_save_tags();
    store_save_schedules();
    screen_manager_load(SCREEN_USER);
}

/* Ask the C6 to erase a tag's bond (LTK/IRK). Runs off the LVGL thread because the SDIO
 * transactions block for up to a few seconds. OP_REMOVE indexes into the C6's connected
 * list, so we resolve that index by MAC; an offline tag has no index and its bond can't
 * be erased this way (the store delete on the UI thread still forgets it locally). We use
 * the store-free tether_ble_unpair() so this task never touches the (single-threaded) store
 * that the LVGL thread owns. `arg` is a heap copy of the 6-byte MAC, freed here. */
static void unpair_task(void *arg)
{
    uint8_t *mac = (uint8_t *)arg;

    tether_conn_t conns[16];
    uint8_t n_conn = 0;
    if (tether_ble_connected(conns, sizeof conns / sizeof conns[0], &n_conn,
                             UNPAIR_CONNECTED_TIMEOUT_MS) == ESP_OK) {
        for (uint8_t i = 0; i < n_conn; i++) {
            if (memcmp(conns[i].mac, mac, TETHER_MAC_LEN) == 0) {
                tether_ble_unpair(i, UNPAIR_REMOVE_TIMEOUT_MS);
                break;
            }
        }
    }

    free(mac);
    vTaskDelete(NULL);
}

static void delete_cb(lv_event_t *e)
{
    (void)e;

    /* Capture the MAC before deleting so a worker task can ask the C6 to erase the bond.
     * The store delete below is the authoritative "forget"; the C6 unpair is best-effort
     * cleanup that only applies while the tag is connected. */
    Tag *tag = NULL;
    if (store_tag_find_by_id(s_tag_id, &tag) == ESP_OK && tag != NULL) {
        uint8_t *mac = malloc(TETHER_MAC_LEN);
        if (mac != NULL) {
            memcpy(mac, tag->mac_address, TETHER_MAC_LEN);
            if (xTaskCreate(unpair_task, "unpair", 4096, mac, 4, NULL) != pdPASS) {
                free(mac);
            }
        }
    }

    /* remove tag from all schedules before deleting */
    Store *st = store_get();
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (st->schedule_id_map[i]) {
            store_schedule_remove_tag(st->schedule_id_map[i]->schedule_id, s_tag_id);
        }
    }
    store_tag_delete(s_tag_id);
    store_save_tags();
    store_save_schedules();
    screen_manager_load(SCREEN_USER);
}

/* ── section label ───────────────────────────────────── */
static lv_obj_t *section_lbl(lv_obj_t *p, const char *txt, const TetherTheme *t)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, t->muted, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_width(l, LV_PCT(100));
    return l;
}

/* ── screen ──────────────────────────────────────────── */
lv_obj_t *tag_edit_screen_create(uint32_t tag_id)
{
    const TetherTheme *t = theme_get();
    Store *st = store_get();

    s_tag_id = tag_id;
    s_selected_schedule_id = UINT32_MAX;
    s_selected_user_id = 0;
    s_kbd = NULL;

    Tag *tag = NULL;
    store_tag_find_by_id(tag_id, &tag);
    strncpy(s_name, tag ? tag->tag_name : "", sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    /* current user assignment */
    if (tag) s_selected_user_id = tag->user_id;

    /* figure out which schedule this tag currently belongs to */
    if (tag) {
        for (int i = 0; i < tag->schedule_count; i++) {
            Schedule *sc = NULL;
            if (store_schedule_find_by_id(tag->schedule_ids[i], &sc) == ESP_OK && sc) {
                s_selected_schedule_id = sc->schedule_id;
                break; /* one schedule per tag */
            }
        }
    }

    /* ── root screen ──────────────────────────────────── */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, t->bg, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* header */
    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 34, 34);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 28, 18);
    lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, t->text, 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_24, 0);
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Edit Item");
    lv_obj_set_style_text_color(title, t->text, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 72, 20);

    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, 960, 1);
    lv_obj_set_style_bg_color(rule, t->line, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 60);

    /* scrollable content */
    int32_t scr_h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, 68);
    lv_obj_set_size(content, LV_PCT(100), scr_h - 68);
    lv_obj_set_style_bg_opa(content, LV_OPA_0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_left(content, 24, 0);
    lv_obj_set_style_pad_right(content, 24, 0);
    lv_obj_set_style_pad_top(content, 20, 0);
    lv_obj_set_style_pad_bottom(content, 24, 0);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                                    LV_FLEX_ALIGN_START,
                                    LV_FLEX_ALIGN_START);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(content, t->scan,   LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content,   LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content,    3,         LV_PART_SCROLLBAR);

    /* ── Name ── */
    section_lbl(content, "NAME", t);

    lv_obj_t *name_card = lv_obj_create(content);
    lv_obj_set_size(name_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(name_card, t->row_bg, 0);
    lv_obj_set_style_border_width(name_card, 0, 0);
    lv_obj_set_style_radius(name_card, 10, 0);
    lv_obj_set_style_pad_all(name_card, 12, 0);
    lv_obj_clear_flag(name_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ta = lv_textarea_create(name_card);
    lv_obj_set_size(ta, LV_PCT(100), 44);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, s_name);
    lv_textarea_set_max_length(ta, 31);
    lv_obj_set_style_bg_color(ta, t->row_bg, 0);
    lv_obj_set_style_border_color(ta, t->line, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_text_color(ta, t->text, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(ta, ta_focused_cb, LV_EVENT_FOCUSED,      NULL);
    lv_obj_add_event_cb(ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── User ── */
    if (st->user_count > 0) {
        section_lbl(content, "USER", t);

        lv_obj_t *user_card = lv_obj_create(content);
        lv_obj_set_size(user_card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(user_card, t->row_bg, 0);
        lv_obj_set_style_border_width(user_card, 0, 0);
        lv_obj_set_style_radius(user_card, 10, 0);
        lv_obj_set_style_pad_all(user_card, 14, 0);
        lv_obj_clear_flag(user_card, LV_OBJ_FLAG_SCROLLABLE);

        /* build user dropdown options */
        char user_opts[256];
        int ulen = 0;
        uint16_t user_sel_idx = 0;
        for (int i = 0; i < st->user_count; i++) {
            if (i > 0) user_opts[ulen++] = '\n';
            ulen += lv_snprintf(user_opts + ulen, sizeof(user_opts) - ulen,
                                "%s", st->users[i].username);
            if (st->users[i].user_id == s_selected_user_id)
                user_sel_idx = (uint16_t)i;
        }

        lv_obj_t *user_dd = lv_dropdown_create(user_card);
        lv_obj_set_width(user_dd, LV_PCT(100));
        lv_dropdown_set_options(user_dd, user_opts);
        lv_dropdown_set_selected(user_dd, user_sel_idx);

        lv_obj_set_style_bg_color(user_dd, t->row_bg, 0);
        lv_obj_set_style_border_color(user_dd, t->line, 0);
        lv_obj_set_style_border_width(user_dd, 1, 0);
        lv_obj_set_style_text_color(user_dd, t->text, 0);
        lv_obj_set_style_text_font(user_dd, &lv_font_montserrat_16, 0);

        lv_obj_t *user_dd_list = lv_dropdown_get_list(user_dd);
        lv_obj_set_style_bg_color(user_dd_list, t->row_bg, 0);
        lv_obj_set_style_border_color(user_dd_list, t->line, 0);
        lv_obj_set_style_text_color(user_dd_list, t->text, 0);
        lv_obj_set_style_text_font(user_dd_list, &lv_font_montserrat_16, 0);
        lv_obj_set_style_bg_color(user_dd_list, t->scan,
                                   LV_PART_SELECTED | LV_STATE_CHECKED);

        lv_obj_add_event_cb(user_dd, user_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* ── Schedule ── */
    section_lbl(content, "SCHEDULE", t);

    lv_obj_t *sched_card = lv_obj_create(content);
    lv_obj_set_size(sched_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(sched_card, t->row_bg, 0);
    lv_obj_set_style_border_width(sched_card, 0, 0);
    lv_obj_set_style_radius(sched_card, 10, 0);
    lv_obj_set_style_pad_all(sched_card, 14, 0);
    lv_obj_clear_flag(sched_card, LV_OBJ_FLAG_SCROLLABLE);

    /* build dropdown options string: "Always On\nSched1\nSched2..." */
    char dd_opts[512];
    int  dd_len = 0;
    dd_len += lv_snprintf(dd_opts + dd_len, sizeof(dd_opts) - dd_len,
                          "%s", NO_SCHEDULE_LABEL);

    uint16_t selected_idx = 0;  /* default = Always On */
    int sched_idx = 1;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        Schedule *sc = st->schedule_id_map[i];
        if (!sc) continue;
        dd_len += lv_snprintf(dd_opts + dd_len, sizeof(dd_opts) - dd_len,
                              "\n%s", sc->schedule_name);
        if (sc->schedule_id == s_selected_schedule_id) {
            selected_idx = (uint16_t)sched_idx;
        }
        sched_idx++;
    }

    lv_obj_t *dd = lv_dropdown_create(sched_card);
    lv_obj_set_width(dd, LV_PCT(100));
    lv_dropdown_set_options(dd, dd_opts);
    lv_dropdown_set_selected(dd, selected_idx);

    /* style the dropdown to match the theme */
    lv_obj_set_style_bg_color(dd, t->row_bg, 0);
    lv_obj_set_style_border_color(dd, t->line, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_text_color(dd, t->text, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_16, 0);

    lv_obj_t *dd_list = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(dd_list, t->row_bg, 0);
    lv_obj_set_style_border_color(dd_list, t->line, 0);
    lv_obj_set_style_text_color(dd_list, t->text, 0);
    lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(dd_list, t->scan,
                               LV_PART_SELECTED | LV_STATE_CHECKED);

    lv_obj_add_event_cb(dd, dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── MAC address (read-only info) ── */
    if (tag) {
        section_lbl(content, "MAC ADDRESS", t);

        lv_obj_t *mac_card = lv_obj_create(content);
        lv_obj_set_size(mac_card, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(mac_card, t->row_bg, 0);
        lv_obj_set_style_border_width(mac_card, 0, 0);
        lv_obj_set_style_radius(mac_card, 10, 0);
        lv_obj_set_style_pad_hor(mac_card, 16, 0);
        lv_obj_set_style_pad_ver(mac_card, 0, 0);
        lv_obj_clear_flag(mac_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        char mac_buf[24];
        const uint8_t *m = tag->mac_address;
        lv_snprintf(mac_buf, sizeof(mac_buf),
                    "%02X:%02X:%02X:%02X:%02X:%02X",
                    m[5], m[4], m[3], m[2], m[1], m[0]);
        lv_obj_t *mac_lbl = lv_label_create(mac_card);
        lv_label_set_text(mac_lbl, mac_buf);
        lv_obj_set_style_text_color(mac_lbl, t->muted, 0);
        lv_obj_set_style_text_font(mac_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(mac_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }

    /* ── Save ── */
    lv_obj_t *save_btn = lv_btn_create(content);
    lv_obj_set_size(save_btn, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(save_btn, t->scan, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_radius(save_btn, 10, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *slbl = lv_label_create(save_btn);
    lv_label_set_text(slbl, "Save");
    lv_obj_set_style_text_color(slbl, t->bg, 0);
    lv_obj_set_style_text_font(slbl, &lv_font_montserrat_16, 0);
    lv_obj_center(slbl);

    /* ── Delete ── */
    lv_obj_t *del_btn = lv_btn_create(content);
    lv_obj_set_size(del_btn, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xC0392B), 0);
    lv_obj_set_style_border_width(del_btn, 0, 0);
    lv_obj_set_style_radius(del_btn, 10, 0);
    lv_obj_set_style_shadow_width(del_btn, 0, 0);
    lv_obj_add_event_cb(del_btn, delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dlbl = lv_label_create(del_btn);
    lv_label_set_text(dlbl, "Remove Item");
    lv_obj_set_style_text_color(dlbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_16, 0);
    lv_obj_center(dlbl);

    /* ── Keyboard ── */
    s_kbd = lv_keyboard_create(scr);
    lv_obj_set_size(s_kbd, LV_PCT(100), LV_PCT(40));
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kbd, ta);
    lv_obj_add_flag(s_kbd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kbd, kbd_ready_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_kbd, kbd_ready_cb, LV_EVENT_CANCEL, NULL);

    return scr;
}

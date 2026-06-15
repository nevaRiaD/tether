#include "user_mgmt.h"
#include "screen_manager.h"
#include "schedule_edit.h"
#include "theme.h"
#include "store.h"
#include "tether_ble.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define SCHEDULE_ALWAYS_ON "Always-On"

/* ── Active user selection ─────────────────────────────────────────── */

static uint32_t s_active_user_id = 0;   /* 0 = not set yet */

uint32_t user_mgmt_get_active_user_id(void)
{
    if (s_active_user_id == 0) {
        Store *st = store_get();
        if (st->user_count > 0)
            s_active_user_id = st->users[0].user_id;
    }
    return s_active_user_id;
}

/* ── Callbacks ─────────────────────────────────────────────────────── */
static void pair_item_cb(lv_event_t *e) {
    (void)e;
    screen_manager_load(SCREEN_TAG_PAIR);
}
static void back_cb(lv_event_t *e) {
    (void)e;
    screen_manager_load(SCREEN_HOME);
}

static void reconnect_task(void *arg)
{
    tether_dev_t devs[8];
    uint8_t n = 0;
    tether_ble_show(devs, 8, &n, 5000);
    vTaskDelete(NULL);
}

static void reconnect_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    lv_label_set_text(lbl, "Scanning...");
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
    xTaskCreate(reconnect_task, "repairscan", 4096, NULL, 5, NULL);
}

static void clear_tags_cb(lv_event_t *e) {
    (void)e;
    Store *st = store_get();
    /* Only clear tags belonging to the active user */
    for (int i = 0; i < MAX_TAGS; i++) {
        Tag *tag = st->tag_id_map[i];
        if (tag && tag->user_id == s_active_user_id)
            store_tag_delete(tag->tag_id);
    }
    screen_manager_reload();
}

static void edit_tag_cb(lv_event_t *e) {
    Tag *tag = (Tag *)lv_event_get_user_data(e);
    if (tag) screen_manager_load_tag_edit(tag->tag_id);
}

static void add_schedule_cb(lv_event_t *e) {
    (void)e;
    screen_manager_load_schedule_edit(UINT32_MAX);
}

static void edit_schedule_cb(lv_event_t *e) {
    Schedule *sched = (Schedule *)lv_event_get_user_data(e);
    if (sched) screen_manager_load_schedule_edit(sched->schedule_id);
}

static void schedule_toggle_cb(lv_event_t *e) {
    Schedule *sched = (Schedule *)lv_event_get_user_data(e);
    if (!sched) return;
    lv_obj_t *sw = lv_event_get_target(e);
    sched->is_active = lv_obj_has_state(sw, LV_STATE_CHECKED);
    store_save_schedules();
}

/* ── User dropdown / add / delete ──────────────────────────────────── */

static void user_dropdown_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    Store *st = store_get();

    if ((int)sel < st->user_count) {
        s_active_user_id = st->users[sel].user_id;
        screen_manager_reload();  /* reload to filter tags */
    }
}

/* ── Add-user dialog ── */
static lv_obj_t *s_add_user_mbox = NULL;
static lv_obj_t *s_add_user_ta   = NULL;
static lv_obj_t *s_add_user_kb   = NULL;

static void add_user_cleanup(void)
{
    if (s_add_user_kb)  { lv_obj_del(s_add_user_kb);  s_add_user_kb  = NULL; }
    if (s_add_user_mbox){ lv_obj_del(s_add_user_mbox); s_add_user_mbox = NULL; }
    s_add_user_ta = NULL;
}

static void add_user_confirm_cb(lv_event_t *e)
{
    (void)e;
    const char *name = s_add_user_ta ? lv_textarea_get_text(s_add_user_ta) : NULL;
    ESP_LOGI("USER_MGMT", "confirm_cb: ta=%p name='%s'", s_add_user_ta, name ? name : "(null)");
    if (name && strlen(name) > 0) {
        User *u = NULL;
        esp_err_t rc = store_user_add(name, 50, 50, &u);
        ESP_LOGI("USER_MGMT", "store_user_add rc=%d user=%p", rc, u);
        if (rc == ESP_OK && u) {
            s_active_user_id = u->user_id;
        }
    }
    add_user_cleanup();
    screen_manager_reload();
}

static void add_user_cancel_cb(lv_event_t *e)
{
    (void)e;
    add_user_cleanup();
}

static void add_user_kb_ready_cb(lv_event_t *e)
{
    (void)e;
    if (s_add_user_kb) lv_obj_add_flag(s_add_user_kb, LV_OBJ_FLAG_HIDDEN);
    /* Shift dialog back to centre */
    if (s_add_user_mbox) lv_obj_align(s_add_user_mbox, LV_ALIGN_CENTER, 0, 0);
}

static void add_user_ta_focus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_add_user_kb) return;
    lv_keyboard_set_textarea(s_add_user_kb, s_add_user_ta);
    lv_obj_clear_flag(s_add_user_kb, LV_OBJ_FLAG_HIDDEN);
    /* Shift dialog up so keyboard doesn't cover it */
    if (s_add_user_mbox) lv_obj_align(s_add_user_mbox, LV_ALIGN_TOP_MID, 0, 20);
}

static void add_user_cb(lv_event_t *e)
{
    (void)e;
    const TetherTheme *t = theme_get();
    lv_obj_t *scr = lv_scr_act();

    /* Overlay */
    s_add_user_mbox = lv_obj_create(scr);
    lv_obj_set_size(s_add_user_mbox, 420, 220);
    lv_obj_align(s_add_user_mbox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_add_user_mbox, t->row_bg, 0);
    lv_obj_set_style_border_color(s_add_user_mbox, t->line, 0);
    lv_obj_set_style_border_width(s_add_user_mbox, 1, 0);
    lv_obj_set_style_radius(s_add_user_mbox, 12, 0);
    lv_obj_set_style_pad_all(s_add_user_mbox, 20, 0);
    lv_obj_clear_flag(s_add_user_mbox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_add_user_mbox);
    lv_label_set_text(title, "New User Profile");
    lv_obj_set_style_text_color(title, t->text, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    s_add_user_ta = lv_textarea_create(s_add_user_mbox);
    lv_obj_set_size(s_add_user_ta, LV_PCT(100), 44);
    lv_textarea_set_one_line(s_add_user_ta, true);
    lv_textarea_set_placeholder_text(s_add_user_ta, "Username");
    lv_textarea_set_max_length(s_add_user_ta, 31);
    lv_obj_set_style_bg_color(s_add_user_ta, t->bg, 0);
    lv_obj_set_style_text_color(s_add_user_ta, t->text, 0);
    lv_obj_set_style_border_color(s_add_user_ta, t->line, 0);
    lv_obj_set_style_border_width(s_add_user_ta, 1, 0);
    lv_obj_align(s_add_user_ta, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *btn_row = lv_obj_create(s_add_user_mbox);
    lv_obj_set_size(btn_row, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 12, 0);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 140, 40);
    lv_obj_set_style_bg_color(cancel_btn, t->line, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, add_user_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(cancel_btn);
    lv_label_set_text(clbl, "Cancel");
    lv_obj_set_style_text_color(clbl, t->text, 0);
    lv_obj_set_style_text_font(clbl, &lv_font_montserrat_14, 0);
    lv_obj_center(clbl);

    lv_obj_t *ok_btn = lv_btn_create(btn_row);
    lv_obj_set_size(ok_btn, 140, 40);
    lv_obj_set_style_bg_color(ok_btn, t->scan, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);
    lv_obj_add_event_cb(ok_btn, add_user_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *olbl = lv_label_create(ok_btn);
    lv_label_set_text(olbl, "Create");
    lv_obj_set_style_text_color(olbl, t->bg, 0);
    lv_obj_set_style_text_font(olbl, &lv_font_montserrat_14, 0);
    lv_obj_center(olbl);

    /* Keyboard — placed on the screen (not inside the mbox) */
    s_add_user_kb = lv_keyboard_create(scr);
    lv_obj_set_style_bg_color(s_add_user_kb, t->row_bg, 0);
    lv_obj_set_style_bg_color(s_add_user_kb, t->line,   LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_add_user_kb, t->text,  LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_add_user_kb, t->bg,  LV_PART_ITEMS);
    lv_keyboard_set_textarea(s_add_user_kb, s_add_user_ta);
    lv_obj_add_flag(s_add_user_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_add_user_kb, add_user_kb_ready_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_add_user_kb, add_user_kb_ready_cb, LV_EVENT_CANCEL, NULL);

    /* Show keyboard when textarea is tapped */
    lv_obj_add_event_cb(s_add_user_ta, add_user_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
}

static void delete_user_cb(lv_event_t *e)
{
    (void)e;
    Store *st = store_get();
    if (st->user_count <= 1) return;  /* don't delete last user */

    store_user_delete(s_active_user_id);
    s_active_user_id = (st->user_count > 0) ? st->users[0].user_id : 0;
    screen_manager_reload();
}

/* ── Formatting helpers ───────────────────────────────────────────── */

static void format_tod(char *buf, size_t len, TimeOfDay tod) {
    int h = (int)tod.hour;
    const char *ap = h >= 12 ? "PM" : "AM";
    if (h > 12) h -= 12;
    if (h == 0) h = 12;
    snprintf(buf, len, "%d:%02d %s", h, (int)tod.minute, ap);
}

static void add_tag_row(lv_obj_t *parent, Tag *tag, const char *schedule,
                        const TetherTheme *t) {
    const char *name = tag ? tag->tag_name : "";
    bool always_on = (strcmp(schedule, SCHEDULE_ALWAYS_ON) == 0);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_bg_color(row, t->row_bg, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_mix(t->row_bg, t->gold, 240), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, edit_tag_cb, LV_EVENT_CLICKED, tag);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, t->green, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, t->text, 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 22, 0);

    lv_obj_t *chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chevron, t->muted, 0);
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_14, 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *sched_lbl = lv_label_create(row);
    lv_label_set_text(sched_lbl, schedule);
    lv_obj_set_style_text_color(sched_lbl, always_on ? t->muted : t->scan, 0);
    lv_obj_set_style_text_font(sched_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(sched_lbl, LV_ALIGN_RIGHT_MID, -20, 0);
}

static void add_schedule_row(lv_obj_t *parent, Schedule *sched, const TetherTheme *t) {
    static const char *day_chars[] = { "M","T","W","T","F","S","S" };
    static const DayMask day_bits[] = {
        DAY_MON, DAY_TUE, DAY_WED, DAY_THU, DAY_FRI, DAY_SAT, DAY_SUN
    };

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, t->row_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, lv_color_mix(t->row_bg, t->gold, 240), LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, edit_schedule_cb, LV_EVENT_CLICKED, sched);

    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_layout(hdr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(hdr);
    lv_label_set_text(name, sched->schedule_name);
    lv_obj_set_style_text_color(name, t->text, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);

    lv_obj_t *sw = lv_switch_create(hdr);
    lv_obj_set_size(sw, 44, 24);
    lv_obj_set_style_bg_color(sw, t->line,   LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, t->scan,   LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, t->text,   LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_MAIN);
    if (sched->is_active) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, schedule_toggle_cb, LV_EVENT_VALUE_CHANGED, sched);

    char start_buf[12], end_buf[12], time_str[28];
    format_tod(start_buf, sizeof(start_buf), sched->start_time);
    format_tod(end_buf,   sizeof(end_buf),   sched->end_time);
    snprintf(time_str, sizeof(time_str), "%s to %s", start_buf, end_buf);

    lv_obj_t *time_lbl = lv_label_create(card);
    lv_label_set_text(time_lbl, time_str);
    lv_obj_set_style_text_color(time_lbl, t->muted, 0);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(time_lbl, LV_PCT(100));

    lv_obj_t *day_row = lv_obj_create(card);
    lv_obj_set_width(day_row, LV_PCT(100));
    lv_obj_set_height(day_row, 22);
    lv_obj_set_style_bg_opa(day_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(day_row, 0, 0);
    lv_obj_set_style_pad_all(day_row, 0, 0);
    lv_obj_set_style_pad_column(day_row, 10, 0);
    lv_obj_set_layout(day_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(day_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(day_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(day_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 7; i++) {
        lv_obj_t *d = lv_label_create(day_row);
        lv_label_set_text(d, day_chars[i]);
        lv_color_t color = (sched->repeat_days & day_bits[i]) ? t->scan : t->muted;
        lv_obj_set_style_text_color(d, color, 0);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
    }
}

static lv_obj_t *section_header(lv_obj_t *parent, const char *text, const TetherTheme *t) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, t->muted, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_obj_set_width(lbl, LV_PCT(100));
    return lbl;
}

static lv_obj_t *empty_state(lv_obj_t *parent, const char *text, const TetherTheme *t) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, t->muted, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(lbl, LV_PCT(100));
    return lbl;
}

lv_obj_t *user_mgmt_screen_create(void) {
    const TetherTheme *t = theme_get();
    Store *st = store_get();

    /* Ensure we have a valid active user */
    user_mgmt_get_active_user_id();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, t->bg, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

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

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "User Management");
    lv_obj_set_style_text_color(title, t->text, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 72, 20);

    /* ── User profile dropdown (top-right) ─────────────────────────── */
    if (st->user_count > 0) {
        lv_obj_t *user_dd = lv_dropdown_create(scr);
        lv_obj_set_size(user_dd, 180, 36);
        lv_obj_align(user_dd, LV_ALIGN_TOP_RIGHT, -80, 16);

        char dd_opts[256];
        int len = 0;
        uint16_t sel_idx = 0;
        for (int i = 0; i < st->user_count; i++) {
            if (i > 0) dd_opts[len++] = '\n';
            len += snprintf(dd_opts + len, sizeof(dd_opts) - len, "%s", st->users[i].username);
            if (st->users[i].user_id == s_active_user_id) sel_idx = (uint16_t)i;
        }
        lv_dropdown_set_options(user_dd, dd_opts);
        lv_dropdown_set_selected(user_dd, sel_idx);

        lv_obj_set_style_bg_color(user_dd, t->row_bg, 0);
        lv_obj_set_style_border_color(user_dd, t->line, 0);
        lv_obj_set_style_border_width(user_dd, 1, 0);
        lv_obj_set_style_text_color(user_dd, t->text, 0);
        lv_obj_set_style_text_font(user_dd, &lv_font_montserrat_14, 0);

        lv_obj_t *dd_list = lv_dropdown_get_list(user_dd);
        lv_obj_set_style_bg_color(dd_list, t->row_bg, 0);
        lv_obj_set_style_border_color(dd_list, t->line, 0);
        lv_obj_set_style_text_color(dd_list, t->text, 0);
        lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_14, 0);
        lv_obj_set_style_bg_color(dd_list, t->scan, LV_PART_SELECTED | LV_STATE_CHECKED);

        lv_obj_add_event_cb(user_dd, user_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* + button to add user */
        lv_obj_t *add_u_btn = lv_btn_create(scr);
        lv_obj_set_size(add_u_btn, 32, 32);
        lv_obj_set_style_radius(add_u_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(add_u_btn, t->scan, 0);
        lv_obj_set_style_border_width(add_u_btn, 0, 0);
        lv_obj_set_style_shadow_width(add_u_btn, 0, 0);
        lv_obj_align(add_u_btn, LV_ALIGN_TOP_RIGHT, -42, 18);
        lv_obj_add_event_cb(add_u_btn, add_user_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *plus = lv_label_create(add_u_btn);
        lv_label_set_text(plus, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_color(plus, t->bg, 0);
        lv_obj_set_style_text_font(plus, &lv_font_montserrat_14, 0);
        lv_obj_center(plus);

        /* trash button to delete current user (only if >1 user) */
        if (st->user_count > 1) {
            lv_obj_t *del_u_btn = lv_btn_create(scr);
            lv_obj_set_size(del_u_btn, 32, 32);
            lv_obj_set_style_radius(del_u_btn, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(del_u_btn, lv_color_hex(0xC0392B), 0);
            lv_obj_set_style_border_width(del_u_btn, 0, 0);
            lv_obj_set_style_shadow_width(del_u_btn, 0, 0);
            lv_obj_align(del_u_btn, LV_ALIGN_TOP_RIGHT, -8, 18);
            lv_obj_add_event_cb(del_u_btn, delete_user_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *trash = lv_label_create(del_u_btn);
            lv_label_set_text(trash, LV_SYMBOL_TRASH);
            lv_obj_set_style_text_color(trash, lv_color_white(), 0);
            lv_obj_set_style_text_font(trash, &lv_font_montserrat_12, 0);
            lv_obj_center(trash);
        }
    } else {
        /* No users — show "Add User" button */
        lv_obj_t *add_u_btn = lv_btn_create(scr);
        lv_obj_set_size(add_u_btn, 120, 32);
        lv_obj_set_style_bg_color(add_u_btn, t->scan, 0);
        lv_obj_set_style_border_width(add_u_btn, 0, 0);
        lv_obj_set_style_radius(add_u_btn, 8, 0);
        lv_obj_set_style_shadow_width(add_u_btn, 0, 0);
        lv_obj_align(add_u_btn, LV_ALIGN_TOP_RIGHT, -28, 18);
        lv_obj_add_event_cb(add_u_btn, add_user_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(add_u_btn);
        lv_label_set_text(lbl, "Add User");
        lv_obj_set_style_text_color(lbl, t->bg, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, 960, 1);
    lv_obj_set_style_bg_color(rule, t->line, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 60);

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
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(content, t->scan,   LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content,   LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content,    3,         LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content,   2,         LV_PART_SCROLLBAR);

    /* add items */
    lv_obj_t *items_header = lv_obj_create(content);
    lv_obj_set_size(items_header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(items_header, LV_OPA_0, 0);
    lv_obj_set_style_border_width(items_header, 0, 0);
    lv_obj_set_style_pad_all(items_header, 0, 0);
    lv_obj_clear_flag(items_header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    
    lv_obj_t *items_cap = lv_label_create(items_header);
    lv_label_set_text(items_cap, "ITEMS");
    lv_obj_set_style_text_color(items_cap, t->muted, 0);
    lv_obj_set_style_text_font(items_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(items_cap, 3, 0);
    lv_obj_align(items_cap, LV_ALIGN_LEFT_MID, 0, 0);

    /* basically same style as schedule button */
	lv_obj_t *add_item_btn = lv_btn_create(items_header);    
	lv_obj_set_size(add_item_btn, 32, 32);
    lv_obj_set_style_radius(add_item_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(add_item_btn, t->scan, 0);
    lv_obj_set_style_border_width(add_item_btn, 0, 0);
    lv_obj_set_style_shadow_width(add_item_btn, 0, 0);
    lv_obj_align(add_item_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(add_item_btn, pair_item_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *item_plus = lv_label_create(add_item_btn);
    lv_label_set_text(item_plus, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(item_plus, t->bg, 0);
    lv_obj_set_style_text_font(item_plus, &lv_font_montserrat_14, 0);
    lv_obj_center(item_plus);

    /* show only tags belonging to the active user */
    int shown = 0;
    for (int i = 0; i < MAX_TAGS; i++) {
        Tag *tag = st->tag_id_map[i];
        if (!tag) continue;
        if (tag->user_id != s_active_user_id) continue;

        const char *sched_name = SCHEDULE_ALWAYS_ON;
        bool found_sched = false;
        for (int s = 0; s < tag->schedule_count && !found_sched; s++) {
            for (int j = 0; j < st->schedule_count; j++) {
                if (st->schedules[j].schedule_id == tag->schedule_ids[s]) {
                    sched_name  = st->schedules[j].schedule_name;
                    found_sched = true;
                    break;
                }
            }
        }
        add_tag_row(content, tag, sched_name, t);
        shown++;
    }

    if (shown == 0) {
        empty_state(content, "No tags paired yet", t);
    }

    lv_obj_t *spacer = lv_obj_create(content);
    lv_obj_set_size(spacer, LV_PCT(100), 6);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_0, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* SCHEDULES header row with "+" button */
    lv_obj_t *sched_hdr = lv_obj_create(content);
    lv_obj_set_size(sched_hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sched_hdr, LV_OPA_0, 0);
    lv_obj_set_style_border_width(sched_hdr, 0, 0);
    lv_obj_set_style_pad_all(sched_hdr, 0, 0);
    lv_obj_clear_flag(sched_hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sched_cap = lv_label_create(sched_hdr);
    lv_label_set_text(sched_cap, "SCHEDULES");
    lv_obj_set_style_text_color(sched_cap, t->muted, 0);
    lv_obj_set_style_text_font(sched_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(sched_cap, 3, 0);
    lv_obj_align(sched_cap, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *add_btn = lv_btn_create(sched_hdr);
    lv_obj_set_size(add_btn, 32, 32);
    lv_obj_set_style_radius(add_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(add_btn, t->scan, 0);
    lv_obj_set_style_border_width(add_btn, 0, 0);
    lv_obj_set_style_shadow_width(add_btn, 0, 0);
    lv_obj_align(add_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(add_btn, add_schedule_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus_lbl = lv_label_create(add_btn);
    lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(plus_lbl, t->bg, 0);
    lv_obj_set_style_text_font(plus_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(plus_lbl);

    if (st->schedule_count == 0) {
        empty_state(content, "No schedules yet", t);
    } else {
        for (int i = 0; i < st->schedule_count; i++) {
            add_schedule_row(content, &st->schedules[i], t);
        }
    }

    lv_obj_t *spacer2 = lv_obj_create(content);
    lv_obj_set_size(spacer2, LV_PCT(100), 6);
    lv_obj_set_style_bg_opa(spacer2, LV_OPA_0, 0);
    lv_obj_set_style_border_width(spacer2, 0, 0);
    lv_obj_clear_flag(spacer2, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_set_size(btn_row, LV_PCT(100), 40);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *reconnect_btn = lv_btn_create(btn_row);
    lv_obj_set_height(reconnect_btn, 40);
    lv_obj_set_flex_grow(reconnect_btn, 1);
    lv_obj_set_style_bg_color(reconnect_btn, t->scan, 0);
    lv_obj_set_style_border_width(reconnect_btn, 0, 0);
    lv_obj_set_style_radius(reconnect_btn, 10, 0);
    lv_obj_set_style_shadow_width(reconnect_btn, 0, 0);
    lv_obj_add_event_cb(reconnect_btn, reconnect_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reconnect_lbl = lv_label_create(reconnect_btn);
    lv_label_set_text(reconnect_lbl, "Reconnect Tags");
    lv_obj_set_style_text_color(reconnect_lbl, t->bg, 0);
    lv_obj_set_style_text_font(reconnect_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(reconnect_lbl);

    lv_obj_t *clear_btn = lv_btn_create(btn_row);
    lv_obj_set_height(clear_btn, 40);
    lv_obj_set_flex_grow(clear_btn, 1);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0xC0392B), 0);
    lv_obj_set_style_border_width(clear_btn, 0, 0);
    lv_obj_set_style_radius(clear_btn, 10, 0);
    lv_obj_set_style_shadow_width(clear_btn, 0, 0);
    lv_obj_add_event_cb(clear_btn, clear_tags_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "Clear All Tags");
    lv_obj_set_style_text_color(clear_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(clear_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(clear_lbl);

    return scr;
}

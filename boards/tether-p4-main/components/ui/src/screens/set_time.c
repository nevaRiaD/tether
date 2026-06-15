#include "set_time.h"
#include "screen_manager.h"
#include "theme.h"
#include "clock.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

/* ── roller handles (file scope so save_cb can read them) ── */
static lv_obj_t *s_roller_hr;
static lv_obj_t *s_roller_min;
static lv_obj_t *s_roller_ampm;
static lv_obj_t *s_roller_mon;
static lv_obj_t *s_roller_day;
static lv_obj_t *s_roller_yr;

/* ── option strings built once at screen create time ── */
static char s_min_opts[184];   /* "00\n01\n...\n59\0" */
static char s_day_opts[96];    /* "01\n02\n...\n31\0" */
static char s_yr_opts[68];     /* "2024\n2025\n...\n2035\0" */

/* ── callbacks ─────────────────────────────────────────── */
static void back_cb(lv_event_t *e) { (void)e; screen_manager_load(SCREEN_SETTINGS); }

static void save_cb(lv_event_t *e)
{
    (void)e;
    int hr12  = (int)lv_roller_get_selected(s_roller_hr)  + 1; /* 1-12  */
    int min   = (int)lv_roller_get_selected(s_roller_min);     /* 0-59  */
    int ampm  = (int)lv_roller_get_selected(s_roller_ampm);    /* 0=AM  */
    int mon   = (int)lv_roller_get_selected(s_roller_mon) + 1; /* 1-12  */
    int day   = (int)lv_roller_get_selected(s_roller_day) + 1; /* 1-31  */
    int yr    = (int)lv_roller_get_selected(s_roller_yr)  + 2024;

    /* 12-h → 24-h:  12 AM = 0, 12 PM = 12, 1 PM = 13 … */
    int hour24 = (hr12 == 12 ? 0 : hr12) + (ampm ? 12 : 0);

    clock_set_manual(yr, mon, day, hour24, min, 0);

    /* persist so next boot restores the manual time */
    nvs_handle_t h;
    if (nvs_open("tether", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, "epoch", (int64_t)time(NULL));
        nvs_commit(h);
        nvs_close(h);
    }
    screen_manager_load(SCREEN_SETTINGS);
}

/* ── roller factory ─────────────────────────────────────── */
static lv_obj_t *make_roller(lv_obj_t *parent,
                               const char *opts,
                               lv_roller_mode_t mode,
                               uint16_t sel,
                               int vis_rows,
                               int width,
                               const TetherTheme *t)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, mode);
    lv_roller_set_visible_row_count(r, vis_rows);
    lv_roller_set_selected(r, sel, LV_ANIM_OFF);
    if (width > 0) lv_obj_set_width(r, width);

    /* blend into the card background */
    lv_obj_set_style_bg_color(r, t->row_bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r,  LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(r, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(r, 0, LV_PART_MAIN);

    /* unselected rows: muted text */
    lv_obj_set_style_text_color(r, t->muted, LV_PART_MAIN);
    lv_obj_set_style_text_font(r,  &lv_font_montserrat_20, LV_PART_MAIN);

    /* selected row: bright text + separator lines (iOS-style) */
    lv_obj_set_style_bg_color(r, t->row_bg, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r,  LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, t->text, LV_PART_SELECTED);
    lv_obj_set_style_text_font(r,  &lv_font_montserrat_20, LV_PART_SELECTED);
    lv_obj_set_style_border_color(r, t->line, LV_PART_SELECTED);
    lv_obj_set_style_border_width(r, 1, LV_PART_SELECTED);
    lv_obj_set_style_border_side(r,
        LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, LV_PART_SELECTED);

    return r;
}

/* ── roller column (roller + field label below) ─────────── */
static lv_obj_t *make_roller_col(lv_obj_t *parent,
                                  const char *opts,
                                  lv_roller_mode_t mode,
                                  uint16_t sel,
                                  int vis_rows,
                                  int width,
                                  const char *field_label,
                                  const TetherTheme *t)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col,     LV_OPA_0, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col,    0, 0);
    lv_obj_set_style_pad_row(col,    6, 0);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *r = make_roller(col, opts, mode, sel, vis_rows, width, t);

    lv_obj_t *lbl = lv_label_create(col);
    lv_label_set_text(lbl, field_label);
    lv_obj_set_style_text_color(lbl, t->muted, 0);
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);

    return r;  /* caller stores the roller handle */
}

/* ── card helper (title + rollers row) ──────────────────── */
static lv_obj_t *make_card(lv_obj_t *parent, const char *title,
                             const TetherTheme *t)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, t->row_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_hor(card, 20, 0);
    lv_obj_set_style_pad_ver(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, title);
    lv_obj_set_style_text_color(cap, t->muted, 0);
    lv_obj_set_style_text_font(cap,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(cap, 3, 0);

    return card;
}

/* ── screen ─────────────────────────────────────────────── */
lv_obj_t *set_time_screen_create(void)
{
    const TetherTheme *t = theme_get();

    /* seed rollers from current system time */
    time_t now_t = time(NULL);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int s_hour = tm_now.tm_hour;
    int s_min  = tm_now.tm_min;
    int s_mon  = tm_now.tm_mon + 1;
    int s_day  = tm_now.tm_mday;
    int s_yr   = tm_now.tm_year + 1900;

    /* derive 12-h values */
    int hr12   = (s_hour % 12 == 0) ? 12 : (s_hour % 12);
    int ampm   = (s_hour >= 12)      ? 1  : 0;

    /* build variable-length option strings */
    int pos = 0;
    for (int i = 0; i < 60; i++)
        pos += snprintf(s_min_opts + pos, sizeof(s_min_opts) - pos,
                        i == 0 ? "%02d" : "\n%02d", i);
    pos = 0;
    for (int i = 1; i <= 31; i++)
        pos += snprintf(s_day_opts + pos, sizeof(s_day_opts) - pos,
                        i == 1 ? "%02d" : "\n%02d", i);
    pos = 0;
    for (int i = 0; i < 12; i++)
        pos += snprintf(s_yr_opts + pos, sizeof(s_yr_opts) - pos,
                        i == 0 ? "%d" : "\n%d", 2024 + i);

    /* ── root screen ── */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, t->bg, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* ── header ── */
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
    lv_label_set_text(title, "Set Time & Date");
    lv_obj_set_style_text_color(title, t->text, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 72, 20);

    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, 960, 1);
    lv_obj_set_style_bg_color(rule, t->line, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 60);

    /* ── content area (flex column) ── */
    int32_t scr_h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, 68);
    lv_obj_set_size(content, LV_PCT(100), scr_h - 68);
    lv_obj_set_style_bg_opa(content, LV_OPA_0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_hor(content, 24, 0);
    lv_obj_set_style_pad_top(content, 20, 0);
    lv_obj_set_style_pad_bottom(content, 24, 0);
    lv_obj_set_style_pad_row(content, 14, 0);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* ── cards row (time | date) ── */
    lv_obj_t *cards_row = lv_obj_create(content);
    lv_obj_set_size(cards_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cards_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(cards_row, 0, 0);
    lv_obj_set_style_pad_all(cards_row, 0, 0);
    lv_obj_set_style_pad_column(cards_row, 14, 0);
    lv_obj_set_layout(cards_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_row,
        LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(cards_row, LV_OBJ_FLAG_SCROLLABLE);

    /* ── TIME card ── */
    lv_obj_t *time_card = make_card(cards_row, "TIME", t);
    lv_obj_set_flex_grow(time_card, 1);
    lv_obj_set_height(time_card, LV_SIZE_CONTENT);

    lv_obj_t *time_rollers = lv_obj_create(time_card);
    lv_obj_set_size(time_rollers, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_rollers, LV_OPA_0, 0);
    lv_obj_set_style_border_width(time_rollers, 0, 0);
    lv_obj_set_style_pad_all(time_rollers, 0, 0);
    lv_obj_set_style_pad_column(time_rollers, 16, 0);
    lv_obj_set_layout(time_rollers, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(time_rollers, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_rollers,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(time_rollers, LV_OBJ_FLAG_SCROLLABLE);

    s_roller_hr   = make_roller_col(time_rollers,
        "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12",
        LV_ROLLER_MODE_INFINITE, (uint16_t)(hr12 - 1), 5, 72, "HOUR", t);
    s_roller_min  = make_roller_col(time_rollers,
        s_min_opts,
        LV_ROLLER_MODE_INFINITE, (uint16_t)s_min, 5, 72, "MIN", t);
    s_roller_ampm = make_roller_col(time_rollers,
        "AM\nPM",
        LV_ROLLER_MODE_NORMAL, (uint16_t)ampm, 5, 72, "AM/PM", t);

    /* ── DATE card ── */
    lv_obj_t *date_card = make_card(cards_row, "DATE", t);
    lv_obj_set_flex_grow(date_card, 1);
    lv_obj_set_height(date_card, LV_SIZE_CONTENT);

    lv_obj_t *date_rollers = lv_obj_create(date_card);
    lv_obj_set_size(date_rollers, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(date_rollers, LV_OPA_0, 0);
    lv_obj_set_style_border_width(date_rollers, 0, 0);
    lv_obj_set_style_pad_all(date_rollers, 0, 0);
    lv_obj_set_style_pad_column(date_rollers, 16, 0);
    lv_obj_set_layout(date_rollers, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(date_rollers, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_rollers,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(date_rollers, LV_OBJ_FLAG_SCROLLABLE);

    s_roller_mon = make_roller_col(date_rollers,
        "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec",
        LV_ROLLER_MODE_INFINITE, (uint16_t)(s_mon - 1), 5, 80, "MONTH", t);
    s_roller_day = make_roller_col(date_rollers,
        s_day_opts,
        LV_ROLLER_MODE_INFINITE, (uint16_t)(s_day - 1), 5, 72, "DAY", t);
    s_roller_yr  = make_roller_col(date_rollers,
        s_yr_opts,
        LV_ROLLER_MODE_NORMAL,
        (uint16_t)(s_yr >= 2024 && s_yr <= 2035 ? s_yr - 2024 : 0),
        5, 88, "YEAR", t);

    /* ── Save button ── */
    lv_obj_t *save_btn = lv_btn_create(content);
    lv_obj_set_size(save_btn, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(save_btn, t->scan, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_radius(save_btn, 12, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save_btn);
    lv_label_set_text(sl, "Save");
    lv_obj_set_style_text_color(sl, t->bg, 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);

    return scr;
}

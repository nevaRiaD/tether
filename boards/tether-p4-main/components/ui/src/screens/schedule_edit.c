#include "schedule_edit.h"
#include "screen_manager.h"
#include "theme.h"
#include "store.h"
#include "models.h"
#include "lvgl.h"
#include <string.h>
#include <stdint.h>

/* ── edit state ───────────────────────────────────────── */
static uint32_t s_edit_id;
static char     s_name[48];
static int      s_sh, s_sm, s_eh, s_em;
static uint8_t  s_days;
static bool     s_active;

static lv_obj_t *s_start_lbl;
static lv_obj_t *s_end_lbl;

static const char    *DAY_LABELS[] = { "M","T","W","T","F","S","S" };
static const DayMask  DAY_BITS[]   = {
    DAY_MON, DAY_TUE, DAY_WED, DAY_THU, DAY_FRI, DAY_SAT, DAY_SUN
};

/* ── time formatting ─────────────────────────────────── */
static void fmt_hm(char *buf, size_t len, int h, int m)
{
    const char *ap = h >= 12 ? "PM" : "AM";
    int dh = h > 12 ? h - 12 : (h == 0 ? 12 : h);
    lv_snprintf(buf, len, "%d:%02d %s", dh, m, ap);
}

static void refresh_start(void) {
    if (!s_start_lbl) return;
    char b[16]; fmt_hm(b, sizeof(b), s_sh, s_sm);
    lv_label_set_text(s_start_lbl, b);
}
static void refresh_end(void) {
    if (!s_end_lbl) return;
    char b[16]; fmt_hm(b, sizeof(b), s_eh, s_em);
    lv_label_set_text(s_end_lbl, b);
}

/* ── adjust button context ───────────────────────────── */
typedef struct { int *val; int delta; int min; int max; void(*refresh)(void); } AdjCtx;
static AdjCtx s_ctx[8];

static void adj_cb(lv_event_t *e)
{
    AdjCtx *c = (AdjCtx *)lv_event_get_user_data(e);
    *c->val += c->delta;
    if (*c->val > c->max) *c->val = c->min;
    if (*c->val < c->min) *c->val = c->max;
    c->refresh();
}

/* ── callbacks ───────────────────────────────────────── */
static void back_cb(lv_event_t *e)   { (void)e; screen_manager_load(SCREEN_USER); }

static void save_cb(lv_event_t *e)
{
    (void)e;
    TimeOfDay st = { (uint16_t)s_sh, (uint16_t)s_sm };
    TimeOfDay et = { (uint16_t)s_eh, (uint16_t)s_em };
    Store *store = store_get();

    if (s_edit_id == UINT32_MAX) {
        uint32_t uid = (store->user_count > 0) ? store->users[0].user_id : 0;
        Schedule *sched = NULL;
        if (store_schedule_add(uid, s_name, st, et, s_days, &sched) == ESP_OK && sched) {
            sched->is_active = s_active;
        }
    } else {
        store_schedule_update_settings(s_edit_id, s_name, st, et, s_days);
        Schedule *sched = NULL;
        if (store_schedule_find_by_id(s_edit_id, &sched) == ESP_OK && sched) {
            sched->is_active = s_active;
        }
    }
    store_save_schedules();
    screen_manager_load(SCREEN_USER);
}

static void delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_edit_id != UINT32_MAX) {
        store_schedule_delete(s_edit_id);
        store_save_schedules();
    }
    screen_manager_load(SCREEN_USER);
}

static void day_cb(lv_event_t *e)
{
    DayMask bit = (DayMask)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    if (lv_obj_has_state(btn, LV_STATE_CHECKED)) s_days |= bit;
    else                                          s_days &= ~bit;
}

static void active_sw_cb(lv_event_t *e)
{
    s_active = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void tag_cb(lv_event_t *e)
{
    if (s_edit_id == UINT32_MAX) return; /* not saved yet */
    Tag *tag = (Tag *)lv_event_get_user_data(e);
    lv_obj_t *cb = lv_event_get_target(e);
    if (lv_obj_has_state(cb, LV_STATE_CHECKED))
        store_schedule_add_tag(s_edit_id, tag->tag_id);
    else
        store_schedule_remove_tag(s_edit_id, tag->tag_id);
    store_save_schedules();
}

/* textarea / keyboard */
static lv_obj_t *s_kbd = NULL;

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

/* ── UI helpers ──────────────────────────────────────── */
static lv_obj_t *make_adj_btn(lv_obj_t *parent, const char *sym,
                                AdjCtx *ctx, const TetherTheme *t)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, t->row_bg, 0);
    lv_obj_set_style_border_color(btn, t->line, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, adj_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, t->text, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *make_time_row(lv_obj_t *parent,
                                 AdjCtx *hplus, AdjCtx *hminus,
                                 AdjCtx *mplus, AdjCtx *mminus,
                                 int h, int m, lv_obj_t **prev_out,
                                 const TetherTheme *t)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    make_adj_btn(row, LV_SYMBOL_LEFT,  hminus, t);
    make_adj_btn(row, LV_SYMBOL_RIGHT, hplus,  t);

    lv_obj_t *sep = lv_label_create(row);
    lv_label_set_text(sep, "hr");
    lv_obj_set_style_text_color(sep, t->muted, 0);
    lv_obj_set_style_text_font(sep, &lv_font_montserrat_14, 0);

    make_adj_btn(row, LV_SYMBOL_LEFT,  mminus, t);
    make_adj_btn(row, LV_SYMBOL_RIGHT, mplus,  t);

    lv_obj_t *sep2 = lv_label_create(row);
    lv_label_set_text(sep2, "min");
    lv_obj_set_style_text_color(sep2, t->muted, 0);
    lv_obj_set_style_text_font(sep2, &lv_font_montserrat_14, 0);

    char buf[16];
    fmt_hm(buf, sizeof(buf), h, m);
    lv_obj_t *prev = lv_label_create(row);
    lv_label_set_text(prev, buf);
    lv_obj_set_style_text_color(prev, t->scan, 0);
    lv_obj_set_style_text_font(prev, &lv_font_montserrat_20, 0);
    *prev_out = prev;

    return row;
}

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
lv_obj_t *schedule_edit_screen_create(uint32_t schedule_id)
{
    const TetherTheme *t = theme_get();
    Store *st = store_get();

    /* defaults */
    s_edit_id = schedule_id;
    s_active  = true;
    s_days    = DAYS_ALL;
    s_sh = 8; s_sm = 0;
    s_eh = 22; s_em = 0;
    strncpy(s_name, "My Schedule", sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_start_lbl = NULL; s_end_lbl = NULL; s_kbd = NULL;

    /* load existing */
    if (schedule_id != UINT32_MAX) {
        Schedule *ex = NULL;
        if (store_schedule_find_by_id(schedule_id, &ex) == ESP_OK && ex) {
            memcpy(s_name, ex->schedule_name, sizeof(s_name) - 1);
            s_sh = ex->start_time.hour;   s_sm = ex->start_time.minute;
            s_eh = ex->end_time.hour;     s_em = ex->end_time.minute;
            s_days   = ex->repeat_days;
            s_active = ex->is_active;
        }
    }

    /* adj contexts:
     * [0]=sh+1 [1]=sh-1 [2]=sm+1 [3]=sm-1
     * [4]=eh+1 [5]=eh-1 [6]=em+1 [7]=em-1 */
    s_ctx[0] = (AdjCtx){ &s_sh, +1, 0, 23, refresh_start };
    s_ctx[1] = (AdjCtx){ &s_sh, -1, 0, 23, refresh_start };
    s_ctx[2] = (AdjCtx){ &s_sm, +1, 0, 59, refresh_start };
    s_ctx[3] = (AdjCtx){ &s_sm, -1, 0, 59, refresh_start };
    s_ctx[4] = (AdjCtx){ &s_eh, +1, 0, 23, refresh_end   };
    s_ctx[5] = (AdjCtx){ &s_eh, -1, 0, 23, refresh_end   };
    s_ctx[6] = (AdjCtx){ &s_em, +1, 0, 59, refresh_end   };
    s_ctx[7] = (AdjCtx){ &s_em, -1, 0, 59, refresh_end   };

    /* root screen */
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

    lv_obj_t *title_lbl = lv_label_create(scr);
    lv_label_set_text(title_lbl, schedule_id == UINT32_MAX ? "New Schedule" : "Edit Schedule");
    lv_obj_set_style_text_color(title_lbl, t->text, 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(title_lbl, 1, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 72, 20);

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
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
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
    lv_textarea_set_max_length(ta, 47);
    lv_obj_set_style_bg_color(ta, t->row_bg, 0);
    lv_obj_set_style_border_color(ta, t->line, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_text_color(ta, t->text, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(ta, ta_focused_cb, LV_EVENT_FOCUSED,       NULL);
    lv_obj_add_event_cb(ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED,  NULL);

    /* ── Start ── */
    section_lbl(content, "START TIME", t);
    lv_obj_t *sc = lv_obj_create(content);
    lv_obj_set_size(sc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(sc, t->row_bg, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_radius(sc, 10, 0);
    lv_obj_set_style_pad_all(sc, 14, 0);
    lv_obj_clear_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    make_time_row(sc, &s_ctx[0], &s_ctx[1], &s_ctx[2], &s_ctx[3],
                  s_sh, s_sm, &s_start_lbl, t);

    /* ── End ── */
    section_lbl(content, "END TIME", t);
    lv_obj_t *ec = lv_obj_create(content);
    lv_obj_set_size(ec, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ec, t->row_bg, 0);
    lv_obj_set_style_border_width(ec, 0, 0);
    lv_obj_set_style_radius(ec, 10, 0);
    lv_obj_set_style_pad_all(ec, 14, 0);
    lv_obj_clear_flag(ec, LV_OBJ_FLAG_SCROLLABLE);
    make_time_row(ec, &s_ctx[4], &s_ctx[5], &s_ctx[6], &s_ctx[7],
                  s_eh, s_em, &s_end_lbl, t);

    /* ── Days ── */
    section_lbl(content, "REPEAT", t);
    lv_obj_t *dc = lv_obj_create(content);
    lv_obj_set_size(dc, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(dc, t->row_bg, 0);
    lv_obj_set_style_border_width(dc, 0, 0);
    lv_obj_set_style_radius(dc, 10, 0);
    lv_obj_set_style_pad_hor(dc, 16, 0);
    lv_obj_set_style_pad_ver(dc, 0, 0);
    lv_obj_set_style_pad_column(dc, 8, 0);
    lv_obj_set_layout(dc, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dc, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 7; i++) {
        lv_obj_t *db = lv_btn_create(dc);
        lv_obj_set_size(db, 44, 36);
        lv_obj_set_style_radius(db, 8, 0);
        lv_obj_set_style_border_width(db, 0, 0);
        lv_obj_set_style_shadow_width(db, 0, 0);
        lv_obj_add_flag(db, LV_OBJ_FLAG_CHECKABLE);
        if (s_days & DAY_BITS[i]) lv_obj_add_state(db, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(db, t->line, LV_PART_MAIN);
        lv_obj_set_style_bg_color(db, t->scan, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_add_event_cb(db, day_cb, LV_EVENT_VALUE_CHANGED,
                             (void *)(uintptr_t)DAY_BITS[i]);
        lv_obj_t *dl = lv_label_create(db);
        lv_label_set_text(dl, DAY_LABELS[i]);
        lv_obj_set_style_text_color(dl, t->text, 0);
        lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
        lv_obj_center(dl);
    }

    /* ── Tags ── */
    if (st->tag_count > 0) {
        section_lbl(content, "ITEMS", t);
        lv_obj_t *tagc = lv_obj_create(content);
        lv_obj_set_size(tagc, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(tagc, t->row_bg, 0);
        lv_obj_set_style_border_width(tagc, 0, 0);
        lv_obj_set_style_radius(tagc, 10, 0);
        lv_obj_set_style_pad_hor(tagc, 16, 0);
        lv_obj_set_style_pad_ver(tagc, 8, 0);
        lv_obj_set_style_pad_row(tagc, 4, 0);
        lv_obj_set_layout(tagc, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(tagc, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(tagc, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < MAX_TAGS; i++) {
            Tag *tag = st->tag_id_map[i];
            if (!tag) continue;

            bool assigned = false;
            if (schedule_id != UINT32_MAX) {
                for (int j = 0; j < tag->schedule_count; j++) {
                    if (tag->schedule_ids[j] == schedule_id) { assigned = true; break; }
                }
            }

            lv_obj_t *crow = lv_obj_create(tagc);
            lv_obj_set_size(crow, LV_PCT(100), 44);
            lv_obj_set_style_bg_opa(crow, LV_OPA_0, 0);
            lv_obj_set_style_border_width(crow, 0, 0);
            lv_obj_set_style_pad_all(crow, 0, 0);
            lv_obj_clear_flag(crow, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *cb = lv_checkbox_create(crow);
            lv_checkbox_set_text(cb, tag->tag_name);
            lv_obj_set_style_text_color(cb, t->text, 0);
            lv_obj_set_style_text_font(cb, &lv_font_montserrat_16, 0);
            lv_obj_set_style_bg_color(cb, t->line, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(cb, t->scan, LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_border_color(cb, t->line, LV_PART_INDICATOR);
            lv_obj_set_style_border_width(cb, 1, LV_PART_INDICATOR);
            lv_obj_align(cb, LV_ALIGN_LEFT_MID, 0, 0);
            if (assigned) lv_obj_add_state(cb, LV_STATE_CHECKED);
            lv_obj_add_event_cb(cb, tag_cb, LV_EVENT_VALUE_CHANGED, tag);
        }
    }

    /* ── Active ── */
    section_lbl(content, "STATUS", t);
    lv_obj_t *ac = lv_obj_create(content);
    lv_obj_set_size(ac, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(ac, t->row_bg, 0);
    lv_obj_set_style_border_width(ac, 0, 0);
    lv_obj_set_style_radius(ac, 10, 0);
    lv_obj_set_style_pad_hor(ac, 16, 0);
    lv_obj_set_style_pad_ver(ac, 0, 0);
    lv_obj_clear_flag(ac, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *al = lv_label_create(ac);
    lv_label_set_text(al, "Schedule Active");
    lv_obj_set_style_text_color(al, t->text, 0);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_16, 0);
    lv_obj_align(al, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *asw = lv_switch_create(ac);
    lv_obj_set_size(asw, 44, 24);
    lv_obj_set_style_bg_color(asw, t->line, LV_PART_MAIN);
    lv_obj_set_style_bg_color(asw, t->scan, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(asw, t->text, LV_PART_KNOB);
    lv_obj_set_style_border_width(asw, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(asw, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(asw, 0, LV_PART_MAIN);
    lv_obj_align(asw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_active) lv_obj_add_state(asw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(asw, active_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Save ── */
    lv_obj_t *save_btn = lv_btn_create(content);
    lv_obj_set_size(save_btn, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(save_btn, t->scan, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_radius(save_btn, 10, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *slbl = lv_label_create(save_btn);
    lv_label_set_text(slbl, "Save Schedule");
    lv_obj_set_style_text_color(slbl, t->bg, 0);
    lv_obj_set_style_text_font(slbl, &lv_font_montserrat_16, 0);
    lv_obj_center(slbl);

    /* ── Delete (edit mode only) ── */
    if (schedule_id != UINT32_MAX) {
        lv_obj_t *del_btn = lv_btn_create(content);
        lv_obj_set_size(del_btn, LV_PCT(100), 48);
        lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xC0392B), 0);
        lv_obj_set_style_border_width(del_btn, 0, 0);
        lv_obj_set_style_radius(del_btn, 10, 0);
        lv_obj_set_style_shadow_width(del_btn, 0, 0);
        lv_obj_add_event_cb(del_btn, delete_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *dlbl = lv_label_create(del_btn);
        lv_label_set_text(dlbl, "Delete Schedule");
        lv_obj_set_style_text_color(dlbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_16, 0);
        lv_obj_center(dlbl);
    }

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

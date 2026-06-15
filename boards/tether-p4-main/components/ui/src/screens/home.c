#include "home.h"
#include "esp_err.h"
#include "screen_manager.h"
#include "theme.h"
#include "store.h"
#include "models.h"
#include "clock.h"
#include "lvgl.h"
#include "nvs.h"
#include "bsp_board_extra.h"  
#include <stdio.h>
#include <string.h>

#define PATH_AUDIO_MISSING      "/audio/beep.wav"
#define DEFAULT_VOLUME_MISSING  50

static bool read_multi_user(void) {
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open("tether", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "multi_user", &val);
        nvs_close(h);
    }
    return val == 1;
}

static void user_cb(lv_event_t *e) {
    (void)e;
    screen_manager_load(SCREEN_USER);
}

static lv_obj_t         *status_dot     = NULL;
static lv_obj_t         *status_text    = NULL;
static lv_obj_t         *missing_panel  = NULL;
static lv_obj_t         *sched_dot_ref  = NULL;
static lv_obj_t         *sched_event    = NULL;
static lv_obj_t         *sched_now_time = NULL;
static lv_timer_t       *s_sched_timer  = NULL;
static bool              s_alert_playing = false; /* true while in a MISSING alert; gates the sound to the entry edge */

static void settings_cb(lv_event_t *e) {
    (void)e;
    screen_manager_load(SCREEN_SETTINGS);
}

static void home_screen_delete_cb(lv_event_t *e) {
    (void)e;
    if (s_sched_timer) {
        lv_timer_delete(s_sched_timer);
        s_sched_timer = NULL;
    }
    status_dot     = NULL;
    status_text    = NULL;
    missing_panel  = NULL;
    sched_dot_ref  = NULL;
    sched_event    = NULL;
    sched_now_time = NULL;
}

static void anim_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void start_opa_pulse(lv_obj_t *target, uint32_t half_ms,
                            lv_opa_t lo, lv_opa_t hi) {
    lv_anim_del(target, anim_opa_cb);
    lv_obj_set_style_opa(target, lo, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, lo, hi);
    lv_anim_set_time(&a, half_ms);
    lv_anim_set_playback_time(&a, half_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void schedule_tick(lv_timer_t *tmr) {
    (void)tmr;

    ClockNow now;
    if (clock_get_now(&now) != ESP_OK) return;

    // format current time for display
    int disp_hour = (int)now.hour;
    const char *ampm = disp_hour >= 12 ? "PM" : "AM";
    if (disp_hour > 12) disp_hour -= 12;
    if (disp_hour == 0) disp_hour = 12;
    char now_str[32];
    snprintf(now_str, sizeof(now_str), "%d:%02d %s", disp_hour, (int)now.minute, ampm);

    // find the soonest upcoming schedule for today
    Store *st = store_get();
    Schedule *next = NULL;
    int best_diff = 25 * 60; // sentinel > 24h
    int now_mins  = (int)now.hour * 60 + (int)now.minute;

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        Schedule *s = st->schedule_id_map[i];
        if (!s) continue;
        if (!(s->repeat_days & now.day)) continue; // not scheduled today

        int sched_mins = (int)s->start_time.hour * 60 + (int)s->start_time.minute;
        int diff = sched_mins - now_mins;
        if (diff < 0) diff += 24 * 60; // already passed today — wrap to next occurrence

        if (diff < best_diff) {
            best_diff = diff;
            next = s;
        }
    }

    if (!next) {
        home_set_schedule("No events today", now_str, "");
        return;
    }

    char countdown[24];
    if (best_diff == 0) {
        snprintf(countdown, sizeof(countdown), "now");
    } else if (best_diff >= 60) {
        snprintf(countdown, sizeof(countdown), "in %dh %dm", best_diff / 60, best_diff % 60);
    } else {
        snprintf(countdown, sizeof(countdown), "in %dm", best_diff);
    }

    home_set_schedule(next->schedule_name, now_str, countdown);
}


void home_set_missing(Tag **missing, int count) {
    if (!missing_panel) return;

    lv_obj_clean(missing_panel);
    if (!missing || count <= 0) {
        home_set_status(TETHER_STATUS_OK);
        return;
    }

    const TetherTheme *t = theme_get();

    uint32_t seen_users[MAX_USERS];
    int unique_user_count = 0;

    for (int i = 0; i < count; i++) {
        if (!missing[i]) continue;
        uint32_t uid = missing[i]->user_id;
        bool already = false;
        for (int j = 0; j < unique_user_count; j++) {
            if (seen_users[j] == uid) { 
            	already = true; 
            	break; 
            }
        }
        if (!already && unique_user_count < MAX_USERS) {
            seen_users[unique_user_count++] = uid;
        }
    }

    bool first_user = false;
    int volume = DEFAULT_VOLUME_MISSING;
    for (int u = 0; u < unique_user_count; u++) {
        uint32_t uid = seen_users[u];

        User *user = NULL;
        esp_err_t err = store_user_find_by_id(uid, &user);

        // set volume from first user detected
        if (err == ESP_OK && !first_user) {
            first_user = true;
            volume = user->audio_loudness;
        }

        lv_obj_t *row = lv_obj_create(missing_panel);
        lv_obj_set_size(row, LV_PCT(100), 34);
        lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, user ? user->username : "Unknown");
        lv_obj_set_style_text_color(name_lbl, t->muted, 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_letter_space(name_lbl, 2, 0);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *items_flex = lv_obj_create(row);
        lv_obj_set_size(items_flex, LV_SIZE_CONTENT, 34);
        lv_obj_set_style_bg_opa(items_flex, LV_OPA_0, 0);
        lv_obj_set_style_border_width(items_flex, 0, 0);
        lv_obj_set_style_pad_all(items_flex, 0, 0);
        lv_obj_set_style_pad_column(items_flex, 24, 0);
        lv_obj_set_layout(items_flex, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(items_flex, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(items_flex, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(items_flex, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(items_flex, LV_ALIGN_LEFT_MID, 130, 0);

        for (int i = 0; i < count; i++) {
            if (!missing[i] || missing[i]->user_id != uid) {
            	continue;
            }
            lv_obj_t *item_lbl = lv_label_create(items_flex);
            lv_label_set_text(item_lbl, missing[i]->tag_name);
            lv_obj_set_style_text_color(item_lbl, t->alert, 0);
            lv_obj_set_style_text_font(item_lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_letter_space(item_lbl, 1, 0);
        }
    }

    //play missing audio — only on the transition into MISSING, not every scan
    if (!s_alert_playing) {
        s_alert_playing = true;
        bsp_extra_codec_volume_set(volume, NULL);
        bsp_extra_player_play_file(PATH_AUDIO_MISSING);
    }
    home_set_status(TETHER_STATUS_MISSING);
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
    lv_obj_align(r, LV_ALIGN_CENTER, 0, 20);
    return r;
}

lv_obj_t *home_screen_create(void) {
 	
    const TetherTheme *t = theme_get();
    bool multi = read_multi_user();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, t->bg, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_add_event_cb(scr, home_screen_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, multi ? "Tether" : "Welcome to Tether, Umar!");
    lv_obj_set_style_text_color(title, t->text, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 32, 20);

    lv_obj_t *user_btn = lv_btn_create(scr);
    lv_obj_set_size(user_btn, 34, 34);
    lv_obj_set_style_radius(user_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(user_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(user_btn, 1, 0);
    lv_obj_set_style_border_color(user_btn, t->text, 0);
    lv_obj_set_style_border_opa(user_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(user_btn, 0, 0);
    lv_obj_align(user_btn, LV_ALIGN_TOP_RIGHT, -66, 18);
    lv_obj_add_event_cb(user_btn, user_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *user_initials = lv_label_create(user_btn);
    lv_label_set_text(user_initials, "US");
    lv_obj_set_style_text_color(user_initials, t->text, 0);
    lv_obj_set_style_text_font(user_initials, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(user_initials, 1, 0);
    lv_obj_center(user_initials);

    lv_obj_t *settings_btn = lv_btn_create(scr);
    lv_obj_set_size(settings_btn, 56, 56);
    lv_obj_set_style_radius(settings_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(settings_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(settings_btn, 0, 0);
    lv_obj_set_style_shadow_width(settings_btn, 0, 0);
    lv_obj_align(settings_btn, LV_ALIGN_TOP_RIGHT, -20, 8);

    lv_obj_t *settings_icon = lv_label_create(settings_btn);
    lv_label_set_text(settings_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(settings_icon, t->text, 0);
    lv_obj_set_style_text_font(settings_icon, &lv_font_montserrat_22, 0);
    lv_obj_add_flag(settings_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(settings_icon, settings_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(settings_icon);
    lv_obj_add_event_cb(settings_btn, settings_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, 960, 1);
    lv_obj_set_style_bg_color(rule, t->line, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 32, 60);

    make_ring(scr, 380, t->gold,   LV_OPA_20);
    make_ring(scr, 300, t->ring,   LV_OPA_40);
    make_ring(scr, 220, t->ring,   LV_OPA_50);
    make_ring(scr, 120, t->scan,   LV_OPA_30);

    status_dot = lv_obj_create(scr);
    lv_obj_clear_flag(status_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(status_dot, 80, 80);
    lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_dot, t->scan, 0);
    lv_obj_set_style_border_width(status_dot, 0, 0);
    lv_obj_align(status_dot, LV_ALIGN_CENTER, 0, 20);

    status_text = lv_label_create(scr);
    lv_label_set_text(status_text, "NO ITEMS DETECTED");
    lv_obj_set_style_text_color(status_text, t->scan, 0);
    lv_obj_set_style_text_font(status_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(status_text, 5, 0);
    lv_obj_align(status_text, LV_ALIGN_CENTER, 0, 90);

    missing_panel = lv_obj_create(scr);
    lv_obj_set_size(missing_panel, 560, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(missing_panel, LV_OPA_0, 0);
    lv_obj_set_style_border_width(missing_panel, 0, 0);
    lv_obj_set_style_pad_all(missing_panel, 0, 0);
    lv_obj_set_style_pad_row(missing_panel, 4, 0);
    lv_obj_set_layout(missing_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(missing_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(missing_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(missing_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(missing_panel, LV_ALIGN_CENTER, 0, 175);
    lv_obj_add_flag(missing_panel, LV_OBJ_FLAG_HIDDEN);

    if (!multi) {
        lv_obj_t *sched_sep = lv_obj_create(scr);
        lv_obj_clear_flag(sched_sep, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(sched_sep, 960, 1);
        lv_obj_set_style_bg_color(sched_sep, t->line, 0);
        lv_obj_set_style_bg_opa(sched_sep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sched_sep, 0, 0);
        lv_obj_set_style_radius(sched_sep, 0, 0);
        lv_obj_align(sched_sep, LV_ALIGN_BOTTOM_LEFT, 32, -37);

        lv_obj_t *sched_right = lv_obj_create(scr);
        lv_obj_clear_flag(sched_right, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(sched_right, LV_SIZE_CONTENT, 18);
        lv_obj_set_style_bg_opa(sched_right, LV_OPA_0, 0);
        lv_obj_set_style_border_width(sched_right, 0, 0);
        lv_obj_set_style_pad_all(sched_right, 0, 0);
        lv_obj_set_style_pad_column(sched_right, 6, 0);
        lv_obj_set_layout(sched_right, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(sched_right, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sched_right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(sched_right, LV_ALIGN_BOTTOM_RIGHT, -32, -10);

        sched_dot_ref = lv_obj_create(sched_right);
        lv_obj_clear_flag(sched_dot_ref, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(sched_dot_ref, 6, 6);
        lv_obj_set_style_radius(sched_dot_ref, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sched_dot_ref, t->scan, 0);
        lv_obj_set_style_bg_opa(sched_dot_ref, LV_OPA_70, 0);
        lv_obj_set_style_border_width(sched_dot_ref, 0, 0);
        lv_obj_set_style_pad_all(sched_dot_ref, 0, 0);

        sched_event = lv_label_create(sched_right);
        lv_label_set_text(sched_event, "Leaving for work in --");
        lv_obj_set_style_text_color(sched_event, t->sched, 0);
        lv_obj_set_style_text_font(sched_event, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(sched_event, 1, 0);

        lv_obj_t *sched_mid = lv_obj_create(scr);
        lv_obj_clear_flag(sched_mid, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(sched_mid, LV_SIZE_CONTENT, 18);
        lv_obj_set_style_bg_opa(sched_mid, LV_OPA_0, 0);
        lv_obj_set_style_border_width(sched_mid, 0, 0);
        lv_obj_set_style_pad_all(sched_mid, 0, 0);
        lv_obj_set_style_pad_column(sched_mid, 6, 0);
        lv_obj_set_layout(sched_mid, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(sched_mid, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sched_mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(sched_mid, LV_ALIGN_BOTTOM_MID, 0, -10);

        lv_obj_t *now_lbl = lv_label_create(sched_mid);
        lv_label_set_text(now_lbl, "NOW");
        lv_obj_set_style_text_color(now_lbl, t->sched, 0);
        lv_obj_set_style_text_font(now_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(now_lbl, 2, 0);

        lv_obj_t *mid_dot = lv_obj_create(sched_mid);
        lv_obj_set_size(mid_dot, 4, 4);
        lv_obj_set_style_radius(mid_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(mid_dot, t->sched, 0);
        lv_obj_set_style_bg_opa(mid_dot, LV_OPA_60, 0);
        lv_obj_set_style_border_width(mid_dot, 0, 0);

        sched_now_time = lv_label_create(sched_mid);
        lv_label_set_text(sched_now_time, "--:-- --");
        lv_obj_set_style_text_color(sched_now_time, t->sched, 0);
        lv_obj_set_style_text_font(sched_now_time, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(sched_now_time, 1, 0);

        s_sched_timer = lv_timer_create(schedule_tick, 60000, NULL);
        schedule_tick(NULL);
    }

    start_opa_pulse(status_dot, 900, LV_OPA_70, LV_OPA_COVER);
    return scr;
}

void home_set_status(tether_status_t status) {
    if (!status_dot || !status_text) return;

    /* Re-arm the alert sound once we leave the MISSING state. */
    if (status != TETHER_STATUS_MISSING) s_alert_playing = false;

    const TetherTheme *t = theme_get();
    lv_color_t color;
    const char *text;
    uint32_t half_ms;
    lv_opa_t lo, hi;

    if (status == TETHER_STATUS_OK) {
        color   = t->green;
        text    = "ALL GOOD";
        half_ms = 1400;
        lo = LV_OPA_70; hi = LV_OPA_COVER;
    } else if (status == TETHER_STATUS_MISSING) {
        color   = t->alert;
        text    = "MISSING ITEMS";
        half_ms = 280;
        lo = LV_OPA_60; hi = LV_OPA_COVER;
    } else {
        color   = t->scan;
        text    = "NO ITEMS DETECTED";
        half_ms = 900;
        lo = LV_OPA_70; hi = LV_OPA_COVER;
    }

    lv_obj_set_style_bg_color(status_dot, color, 0);
    lv_obj_set_style_text_color(status_text, color, 0);
    lv_label_set_text(status_text, text);
    if (missing_panel) {
        if (status == TETHER_STATUS_MISSING)
            lv_obj_clear_flag(missing_panel, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(missing_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (sched_dot_ref) lv_obj_set_style_bg_color(sched_dot_ref, color, 0);

    start_opa_pulse(status_dot, half_ms, lo, hi);
}

void home_set_schedule(const char *event, const char *now_time, const char *countdown) {
    if (sched_now_time) lv_label_set_text(sched_now_time, now_time);
    if (sched_event) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s", event, countdown);
        lv_label_set_text(sched_event, buf);
    }
}

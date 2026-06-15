#include "settings.h"
#include "screen_manager.h"
#include "theme.h"
#include "store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#define NVS_NS        "tether"
#define NVS_MULTI     "multi_user"

static bool nvs_load_multi_user(void) {
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_MULTI, &val);
        nvs_close(h);
    }
    return val == 1;
}

static void nvs_save_multi_user(bool on) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_MULTI, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

typedef struct {
    lv_obj_t *pct_label;
    bool      is_volume;
} SliderCtx;

static SliderCtx s_vol_ctx;
static SliderCtx s_bright_ctx;

static void back_cb(lv_event_t *e) {
    (void)e;
    screen_manager_load(SCREEN_HOME);
}

static void test_audio_cb(lv_event_t *slider) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_user_data(slider);
    int volume = sl ? (int)lv_slider_get_value(sl) : 50;
    bsp_extra_codec_volume_set(volume, NULL);
    bsp_extra_player_play_file("/audio/beep.wav");
}

static void slider_store_cb(lv_event_t *e) {
    lv_obj_t  *sl  = lv_event_get_target(e);
    SliderCtx *ctx = (SliderCtx *)lv_obj_get_user_data(sl);
    int        val = (int)lv_slider_get_value(sl);

    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%d%%", val);
    lv_label_set_text(ctx->pct_label, buf);

    if (!ctx->is_volume) {
        bsp_display_brightness_set(val < 10 ? 10 : val);
    }

    Store *st = store_get();
    if (st->user_count > 0) {
        User    *u      = &st->users[0];
        uint8_t  loud   = ctx->is_volume ? (uint8_t)val : u->audio_loudness;
        uint8_t  bright = ctx->is_volume ? u->alert_brightness : (uint8_t)val;
        store_user_update_settings(u->user_id, loud, bright);
        store_save_users();
    }
}

static void dark_mode_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    theme_set_dark(lv_obj_has_state(sw, LV_STATE_CHECKED));
    screen_manager_reload();
}

static void multi_user_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    nvs_save_multi_user(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void set_time_row_cb(lv_event_t *e) {
    (void)e;
    screen_manager_load(SCREEN_SET_TIME);
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *title,
                           const char *subtitle, int y) {
    const TetherTheme *t = theme_get();

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 960, subtitle ? 72 : 56);
    lv_obj_set_style_bg_color(row, t->row_bg, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_left(row, 20, 0);
    lv_obj_set_style_pad_right(row, 20, 0);
    lv_obj_set_style_pad_top(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, t->text, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, subtitle ? -12 : 0);

    if (subtitle) {
        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_text(sub, subtitle);
        lv_obj_set_style_text_color(sub, t->muted, 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 0, 14);
    }

    return row;
}

static lv_obj_t *add_slider(lv_obj_t *row, int value, SliderCtx *ctx) {
    const TetherTheme *t = theme_get();

    lv_obj_t *pct = lv_label_create(row);
    lv_obj_set_style_text_color(pct, t->muted, 0);
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_12, 0);
    lv_obj_align(pct, LV_ALIGN_RIGHT_MID, 0, 12);
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%d%%", value);
    lv_label_set_text(pct, buf);
    ctx->pct_label = pct;

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_size(sl, 160, 4);
    lv_obj_set_style_bg_color(sl, t->line, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, t->gold, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, t->text, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);
    lv_slider_set_value(sl, value, LV_ANIM_OFF);
    lv_obj_align(sl, LV_ALIGN_RIGHT_MID, 0, -10);
    lv_obj_set_user_data(sl, ctx);
    return sl;
}

static lv_obj_t *add_switch(lv_obj_t *row, bool on) {
    const TetherTheme *t = theme_get();

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 28);
    lv_obj_set_style_bg_color(sw, t->line, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, t->gold, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, t->text, LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 2, LV_PART_KNOB);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

lv_obj_t *settings_screen_create(void) {
    const TetherTheme *t = theme_get();

    Store *st         = store_get();
    int    init_vol   = (st->user_count > 0) ? st->users[0].audio_loudness   : 70;
    int    init_bright= (st->user_count > 0) ? st->users[0].alert_brightness : 100;

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

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, t->text, 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_24, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Settings");
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

    lv_obj_t *row;
    lv_obj_t *sl;
    lv_obj_t *sw;

    s_vol_ctx.is_volume = true;
    row = make_row(scr, "Alert Volume", "Sound played when items are missing", 80);
    sl  = add_slider(row, init_vol, &s_vol_ctx);
    lv_obj_add_event_cb(sl, slider_store_cb, LV_EVENT_VALUE_CHANGED, NULL);
	
	/* volume test button */ 
    lv_obj_t *test_btn = lv_btn_create(row);
    lv_obj_set_size(test_btn, 56, 28);
    lv_obj_set_style_bg_color(test_btn, t->scan, 0);
    lv_obj_set_style_border_width(test_btn, 0, 0);
    lv_obj_set_style_radius(test_btn, 6, 0);
    lv_obj_set_style_shadow_width(test_btn, 0, 0);
    lv_obj_align(test_btn, LV_ALIGN_RIGHT_MID, -200, 0);
    lv_obj_add_event_cb(test_btn, test_audio_cb, LV_EVENT_CLICKED, sl);
    lv_obj_t *test_lbl = lv_label_create(test_btn);
    lv_label_set_text(test_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(test_lbl, t->bg, 0);
    lv_obj_set_style_text_font(test_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(test_lbl);

    s_bright_ctx.is_volume = false;
    row = make_row(scr, "Display Brightness", "Adjust screen brightness", 172);
    sl  = add_slider(row, init_bright, &s_bright_ctx);
    lv_obj_add_event_cb(sl, slider_store_cb, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_row(scr, "Dark Mode", "Switch between dark and light themes", 264);
    sw  = add_switch(row, theme_is_dark());
    lv_obj_add_event_cb(sw, dark_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_row(scr, "Multi-User Mode", "Track items per household member", 356);
    sw  = add_switch(row, nvs_load_multi_user());
    lv_obj_add_event_cb(sw, multi_user_cb, LV_EVENT_VALUE_CHANGED, NULL);

    row = make_row(scr, "Set Time & Date", "Correct device clock", 448);
    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, t->muted, 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_16, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, t->row_bg, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, set_time_row_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

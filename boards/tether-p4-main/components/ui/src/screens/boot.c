#include "boot.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "screen_manager.h"
#include "screens/home.h"
#include "theme.h"
#include "lvgl.h"
#include "src/libs/gif/lv_gif.h"
#include "bsp_board_extra.h"

extern const uint8_t boot_small_gif[];
extern const unsigned int boot_small_gif_len;

#define BOOT_DURATION_MS    7500
#define PATH_AUDIO_STARTUP  "/audio/beep.wav"

static lv_timer_t *s_boot_timer = NULL;

static void boot_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    lv_timer_delete(s_boot_timer);
    s_boot_timer = NULL;
    screen_manager_load(SCREEN_HOME);
    home_set_status(TETHER_STATUS_UNKNOWN);
}

static void boot_screen_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_boot_timer) {
        lv_timer_delete(s_boot_timer);
        s_boot_timer = NULL;
    }
}

lv_obj_t *boot_screen_create(void)
{
	bsp_extra_codec_volume_set(25, NULL);
    for (int i = 0; i < 3; i++) {
        bsp_extra_player_play_file(PATH_AUDIO_STARTUP);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
	
    const TetherTheme *t = theme_get();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, t->bg, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_add_event_cb(scr, boot_screen_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WELCOME TO TETHER");
    lv_obj_set_style_text_color(title, t->text, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(title, 6, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    static lv_img_dsc_t gif_dsc;
    gif_dsc.header.cf        = LV_COLOR_FORMAT_RAW;
    gif_dsc.header.w         = 0;
    gif_dsc.header.h         = 0;
    gif_dsc.data_size        = boot_small_gif_len;
    gif_dsc.data             = boot_small_gif;

    lv_obj_t *gif = lv_gif_create(scr);
    lv_gif_set_src(gif, &gif_dsc);
    lv_obj_align(gif, LV_ALIGN_CENTER, 0, 40);

    s_boot_timer = lv_timer_create(boot_timer_cb, BOOT_DURATION_MS, NULL);
    lv_timer_set_repeat_count(s_boot_timer, 1);

    return scr;
}

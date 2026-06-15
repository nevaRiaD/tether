#include "theme.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NS       "tether"
#define NVS_KEY_DARK "dark_mode"

static TetherTheme  s_dark;
static TetherTheme  s_light;
static bool         s_ready  = false;
static TetherTheme *s_active = NULL;

static void build_themes(void) {
    if (s_ready) return;

    s_dark = (TetherTheme){
        .bg       = lv_color_hex(0x0B1829),
        .text     = lv_color_hex(0xE8F0FF),
        .line     = lv_color_hex(0x1C3050),
        .muted    = lv_color_hex(0x4A6480),
        .gold     = lv_color_hex(0xC8A035),
        .row_bg   = lv_color_hex(0x0F2035),
        .ring     = lv_color_hex(0x2A4468),
        .scan     = lv_color_hex(0x5A8ABB),
        .alert    = lv_color_hex(0xBD3044),
        .green    = lv_color_hex(0x3A9E6A),
        .sched    = lv_color_hex(0x5E80A0),
        .gold_dim = lv_color_hex(0x9E7E28),
        .is_dark  = true,
    };

    s_light = (TetherTheme){
        .bg       = lv_color_hex(0xF0F5FF),
        .text     = lv_color_hex(0x0B1829),
        .line     = lv_color_hex(0xC8D8EC),
        .muted    = lv_color_hex(0x7A94B0),
        .gold     = lv_color_hex(0xC8A035),
        .row_bg   = lv_color_hex(0xE2EBF8),
        .ring     = lv_color_hex(0xA8BDD8),
        .scan     = lv_color_hex(0x4070A0),
        .alert    = lv_color_hex(0xBD3044),
        .green    = lv_color_hex(0x2D8A5C),
        .sched    = lv_color_hex(0x4A6E90),
        .gold_dim = lv_color_hex(0x9E7E28),
        .is_dark  = false,
    };

    s_active = &s_dark;
    s_ready  = true;
}

static void nvs_save_dark(bool dark) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_DARK, dark ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_load_dark(void) {
    nvs_handle_t h;
    uint8_t val = 1;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_DARK, &val);
        nvs_close(h);
    }
    return val == 1;
}

void theme_init(void) {
    build_themes();
    s_active = nvs_load_dark() ? &s_dark : &s_light;
}

const TetherTheme *theme_get(void) {
    build_themes();
    return s_active;
}

void theme_set_dark(bool dark) {
    build_themes();
    s_active = dark ? &s_dark : &s_light;
    nvs_save_dark(dark);
}

bool theme_is_dark(void) {
    build_themes();
    return s_active->is_dark;
}

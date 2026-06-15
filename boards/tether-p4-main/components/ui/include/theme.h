#pragma once

#include "lvgl.h"
#include <stdbool.h>

typedef struct {
    lv_color_t bg;
    lv_color_t text;
    lv_color_t line;
    lv_color_t muted;
    lv_color_t gold;
    lv_color_t row_bg;
    lv_color_t ring;
    lv_color_t scan;
    lv_color_t alert;
    lv_color_t green;
    lv_color_t sched;
    lv_color_t gold_dim;
    bool       is_dark;
} TetherTheme;

void               theme_init(void);
const TetherTheme *theme_get(void);
void               theme_set_dark(bool dark);
bool               theme_is_dark(void);

#pragma once

#include "lvgl.h"
#include "tether.h"
#include "models.h"

lv_obj_t *home_screen_create(void);
void home_set_status(tether_status_t status);
void home_set_schedule(const char *event, const char *time, const char *countdown);
void home_set_missing(Tag **missing, int count);

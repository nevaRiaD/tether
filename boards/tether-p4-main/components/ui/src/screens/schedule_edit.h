#pragma once
#include "lvgl.h"
#include <stdint.h>

/* Pass UINT32_MAX to create a new schedule; pass an existing schedule_id to edit. */
lv_obj_t *schedule_edit_screen_create(uint32_t schedule_id);

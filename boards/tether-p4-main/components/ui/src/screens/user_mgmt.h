#pragma once
#include "lvgl.h"
#include <stdint.h>

lv_obj_t *user_mgmt_screen_create(void);

/* Returns the currently selected user profile ID.
 * Falls back to the first user if none selected yet. */
uint32_t user_mgmt_get_active_user_id(void);

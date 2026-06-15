#pragma once

typedef enum {
	SCREEN_BOOT = 0,
    SCREEN_HOME,
    SCREEN_USER,
    SCREEN_SETTINGS,
    SCREEN_TAG_PAIR,
    SCREEN_SET_TIME,
    SCREEN_SCHEDULE_EDIT,
    SCREEN_TAG_EDIT,
    SCREEN_COUNT
} screen_id_t;

#include <stdint.h>

void        screen_manager_load(screen_id_t id);
void        screen_manager_load_schedule_edit(uint32_t schedule_id);
void        screen_manager_load_tag_edit(uint32_t tag_id);
void        screen_manager_reload(void);   // re-create current screen (e.g. after theme change)
screen_id_t screen_manager_current(void);

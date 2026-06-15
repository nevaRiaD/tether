#include "screen_manager.h"
#include "screens/home.h"
#include "screens/settings.h"
#include "screens/tag_pair.h"
#include "screens/user_mgmt.h"
#include "screens/set_time.h"
#include "screens/schedule_edit.h"
#include "screens/tag_edit.h"
#include "screens/boot.h"
#include "lvgl.h"
#include <stdint.h>

static screen_id_t current_screen     = SCREEN_COUNT;
static uint32_t    s_schedule_edit_id = UINT32_MAX;
static uint32_t    s_tag_edit_id      = UINT32_MAX;

void screen_manager_load(screen_id_t id) {
    if (id == current_screen) return;

    lv_obj_t *scr = NULL;
    switch (id) {
        case SCREEN_HOME:          scr = home_screen_create();                         break;
        case SCREEN_BOOT:		   scr = boot_screen_create();                        break;
        case SCREEN_USER:          scr = user_mgmt_screen_create();                    break;
        case SCREEN_SETTINGS:      scr = settings_screen_create();                     break;
        case SCREEN_TAG_PAIR:      scr = tag_pair_screen_create();                     break;
        case SCREEN_SET_TIME:      scr = set_time_screen_create();                     break;
        case SCREEN_SCHEDULE_EDIT: scr = schedule_edit_screen_create(s_schedule_edit_id);
                                   s_schedule_edit_id = UINT32_MAX;                    break;
        case SCREEN_TAG_EDIT:      scr = tag_edit_screen_create(s_tag_edit_id);
                                   s_tag_edit_id = UINT32_MAX;                         break;
        default: return;
    }

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
    current_screen = id;
}

void screen_manager_load_schedule_edit(uint32_t schedule_id) {
    s_schedule_edit_id = schedule_id;
    current_screen = SCREEN_COUNT; /* force reload even if already on this screen */
    screen_manager_load(SCREEN_SCHEDULE_EDIT);
}

void screen_manager_load_tag_edit(uint32_t tag_id) {
    s_tag_edit_id = tag_id;
    current_screen = SCREEN_COUNT;
    screen_manager_load(SCREEN_TAG_EDIT);
}

void screen_manager_reload(void) {
    screen_id_t id = current_screen;
    current_screen = SCREEN_COUNT;
    screen_manager_load(id);
}

screen_id_t screen_manager_current(void) {
    return current_screen;
}

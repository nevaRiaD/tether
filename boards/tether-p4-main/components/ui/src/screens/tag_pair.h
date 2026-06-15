#pragma once
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

lv_obj_t *tag_pair_screen_create(void);

/*
 * Called by sdio_event.c (in the tether-ble task) when TETHER_BLE_EVT_PAIR_COMPLETE
 * arrives. If the pair screen is active it saves the tag and navigates home;
 * otherwise it is a no-op.
 */
void tag_pair_on_pair_complete(const uint8_t mac[6], bool success);
void tag_pair_on_incoming_pair(const uint8_t mac[6]);

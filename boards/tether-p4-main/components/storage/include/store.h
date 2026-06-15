#pragma once
// store.h — cJSON-backed persistence API
// Each entity group is saved to its own JSON file.
// The full Store lives in RAM; call store_save_* to flush to disk.

#ifdef HOST_BUILD
  #include "esp_stub.h"
#else
  #include "esp_err.h"
  #include "esp_log.h"
#endif

#include "models.h"
#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

// Initialise the store and load all JSON files from base_dir.
// base_dir: directory path, e.g. "/littlefs" on ESP32 or "test_data" on Windows.
// Missing files are treated as empty (first boot).
esp_err_t store_init(const char *base_dir);

// Flush all data back to JSON files.
esp_err_t store_save_all(void);

// Get a pointer to the global Store (read/modify then call save).
Store *store_get(void);

// ─────────────────────────────────────────────────────────────────────────────
// Users
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t store_user_add(const char *username, uint8_t loudness,
                          uint8_t brightness, User **out);
esp_err_t store_user_find_by_id(uint32_t user_id, User **out);
esp_err_t store_user_find_by_name(const char *username, User **out);
esp_err_t store_user_update_settings(uint32_t user_id,
                                      uint8_t loudness, uint8_t brightness);
esp_err_t store_user_delete(uint32_t user_id);

// ─────────────────────────────────────────────────────────────────────────────
// Tags
// ─────────────────────────────────────────────────────────────────────────────
// Persistent config
esp_err_t store_tag_add(const uint8_t mac[6], const char *name, Tag **out, uint32_t user_id);
esp_err_t store_tag_find_by_id(uint32_t tag_id, Tag **out);
esp_err_t store_tag_find_by_mac(const uint8_t mac[6], Tag **out);
esp_err_t store_tag_delete(uint32_t tag_id);
esp_err_t store_tag_update_settings(uint32_t tag_id, char *tag_name, uint32_t user_id);
// ─────────────────────────────────────────────────────────────────────────────
// Schedules
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t store_schedule_add(uint32_t user_id, const char *name,
                              TimeOfDay start, TimeOfDay end,
                              DayMask days, Schedule **out);
esp_err_t store_schedule_find_by_id(uint32_t schedule_id, Schedule **out);
esp_err_t store_schedule_add_tag(uint32_t schedule_id, uint32_t tag_id);
esp_err_t store_schedule_remove_tag(uint32_t schedule_id, uint32_t tag_id);
esp_err_t store_schedule_delete(uint32_t schedule_id);
esp_err_t store_schedule_update_settings(uint32_t schedule_id,char *schedule_name, TimeOfDay start_time, TimeOfDay end_time, DayMask days);
// Returns pointers to schedules active at the given hour/minute/day.
// out_ptrs must be an array of at least max Schedule* pointers.
esp_err_t store_schedule_get_active(uint16_t hour, uint16_t minute,
                                     DayMask day,
                                     Schedule **out_ptrs, int max, int *count);

// ─────────────────────────────────────────────────────────────────────────────
// Individual save helpers (call after modifying a section)
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t store_save_users(void);
esp_err_t store_save_tags(void);
esp_err_t store_save_schedules(void);

#pragma once
// models.h — BLE Tracker data models
// Shared between ESP32 (ESP-IDF) and Windows native (HOST_BUILD)

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// ─────────────────────────────────────────────────────────────────────────────
// LIMITS — maximum counts stored in memory
// ─────────────────────────────────────────────────────────────────────────────
#define MAX_USERS       8
#define MAX_TAGS        32
#define MAX_SCHEDULES   16
#define HASH_SIZE 64 
#define MAX_SCHEDULES_PER_TAG 8
// ─────────────────────────────────────────────────────────────────────────────
// USER
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    uint32_t user_id;           // unique ID (auto-assigned)
    char     username[32];      // unique, null-terminated
    uint8_t  audio_loudness;    // 0–100
    uint8_t  alert_brightness;  // 0–100
} User;

// ─────────────────────────────────────────────────────────────────────────────
// TAG
// ─────────────────────────────────────────────────────────────────────────────
// Persisted to tags.json
typedef enum {
    TAG_TYPE_RSSI  = 0,   /* connection-based: RSSI + Kalman motion detection */
    TAG_TYPE_ACCEL = 1,   /* advertising-only: accelerometer mfr-data detection */
} TagType;

typedef struct {
    uint32_t user_id; //implement this
    bool     occupied;
    uint32_t tag_id;
    uint8_t  mac_address[6];
    char     tag_name[32];
    uint32_t schedule_ids[MAX_SCHEDULES_PER_TAG];
    int      schedule_count;
    uint32_t tag_collection_mask; // implement this
    TagType  tag_type;            /* TAG_TYPE_RSSI or TAG_TYPE_ACCEL */
} Tag;

typedef struct {
    Tag buckets[HASH_SIZE];
} TagHashTable;
// ─────────────────────────────────────────────────────────────────────────────
// SCHEDULE
// ─────────────────────────────────────────────────────────────────────────────
typedef uint8_t DayMask;
#define DAY_MON      (1 << 0)
#define DAY_TUE      (1 << 1)
#define DAY_WED      (1 << 2)
#define DAY_THU      (1 << 3)
#define DAY_FRI      (1 << 4)
#define DAY_SAT      (1 << 5)
#define DAY_SUN      (1 << 6)
#define DAYS_WEEKDAY (DAY_MON | DAY_TUE | DAY_WED | DAY_THU | DAY_FRI)
#define DAYS_WEEKEND (DAY_SAT | DAY_SUN)
#define DAYS_ALL     (0x7F)

typedef struct {
    uint16_t hour;    // 0–23
    uint16_t minute;  // 0–59
} TimeOfDay;

typedef struct {
    uint32_t  schedule_id;
    uint32_t  user_id;                              // owning user
    char      schedule_name[48];
    TimeOfDay start_time;
    TimeOfDay end_time;
    DayMask   repeat_days;
    bool      is_active;
    uint32_t  required_mask;                        // bitmask of associated tag_ids
} Schedule;

// ─────────────────────────────────────────────────────────────────────────────
// STORE — top-level in-memory state (what gets serialized to JSON)
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    User        users[MAX_USERS];
    int         user_count;

    TagHashTable mac_index;
    Tag         *tag_id_map[MAX_TAGS];
    uint32_t    used_slots_mask;
    int         tag_count;

    Schedule    *schedule_id_map[MAX_SCHEDULES];
    uint32_t    used_schedules_mask;
    Schedule    schedules[MAX_SCHEDULES];
    int         schedule_count;
 
} Store;
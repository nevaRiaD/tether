// store.c — cJSON-backed persistence layer
// Compiles on ESP32 (ESP-IDF) and Windows (HOST_BUILD via PlatformIO native).

#ifdef HOST_BUILD
  #include "esp_stub.h"
#else
  #include "esp_err.h"
  #include "esp_log.h"
#endif

#include "store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "store";

// ─────────────────────────────────────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────────────────────────────────────

static Store     s_store;
static char      s_base_dir[64];
static uint32_t  s_next_user_id     = 1;


// ─────────────────────────────────────────────────────────────────────────────
// Internal file helpers
// ─────────────────────────────────────────────────────────────────────────────

static void make_path(char *buf, size_t buf_size, const char *filename) {
    snprintf(buf, buf_size, "%s/%s", s_base_dir, filename);
}

// Read entire file into a heap-allocated string. Caller must free().
// Returns NULL if file doesn't exist (treated as empty, not an error).
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static esp_err_t write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open for write: %s", path);
        return ESP_FAIL;
    }
    fputs(content, f);
    fclose(f);
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// User serialization
// ─────────────────────────────────────────────────────────────────────────────

static cJSON *user_to_json(const User *u) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "user_id",          (double)u->user_id);
    cJSON_AddStringToObject(obj, "username",         u->username);
    cJSON_AddNumberToObject(obj, "audio_loudness",   u->audio_loudness);
    cJSON_AddNumberToObject(obj, "alert_brightness", u->alert_brightness);
    return obj;
}

static void json_to_user(const cJSON *obj, User *out) {
    out->user_id          = (uint32_t)cJSON_GetObjectItem(obj, "user_id")->valuedouble;
    out->audio_loudness   = (uint8_t) cJSON_GetObjectItem(obj, "audio_loudness")->valueint;
    out->alert_brightness = (uint8_t) cJSON_GetObjectItem(obj, "alert_brightness")->valueint;
    strncpy(out->username,
            cJSON_GetObjectItem(obj, "username")->valuestring,
            sizeof(out->username) - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tag serialization
// ─────────────────────────────────────────────────────────────────────────────

static cJSON *tag_to_json(const Tag *t) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "tag_id",             (double)t->tag_id);
    cJSON_AddNumberToObject(obj, "user_id",            (double)t->user_id);
    cJSON_AddStringToObject(obj, "tag_name",           t->tag_name);
    cJSON *mac_arr = cJSON_CreateArray();
    for (int i = 0; i < 6; i++)
        cJSON_AddItemToArray(mac_arr, cJSON_CreateNumber(t->mac_address[i]));
    cJSON_AddItemToObject(obj, "mac_address", mac_arr);

    cJSON *sched_arr = cJSON_CreateArray();
    for (int i = 0; i < t->schedule_count; i++)
        cJSON_AddItemToArray(sched_arr, cJSON_CreateNumber((double)t->schedule_ids[i]));
    cJSON_AddItemToObject(obj, "schedule_ids", sched_arr);

    cJSON_AddNumberToObject(obj, "tag_type", (double)t->tag_type);

    return obj;
}


static void json_to_tag(const cJSON *obj, Tag *out) {
    memset(out, 0, sizeof(*out));
    out->tag_id             = (uint32_t)cJSON_GetObjectItem(obj, "tag_id")->valuedouble;
    cJSON *uid_item = cJSON_GetObjectItem(obj, "user_id");
    out->user_id = uid_item ? (uint32_t)uid_item->valuedouble : 0;
    strncpy(out->tag_name,
            cJSON_GetObjectItem(obj, "tag_name")->valuestring,
            sizeof(out->tag_name) - 1);

    cJSON *mac_arr = cJSON_GetObjectItem(obj, "mac_address");
    if (mac_arr && cJSON_GetArraySize(mac_arr) == 6)
        for (int i = 0; i < 6; i++)
            out->mac_address[i] = (uint8_t)cJSON_GetArrayItem(mac_arr, i)->valueint;

    cJSON *sched_arr = cJSON_GetObjectItem(obj, "schedule_ids");
    out->schedule_count = 0;
    if (sched_arr) {
        int n = cJSON_GetArraySize(sched_arr);
        for (int i = 0; i < n && i < MAX_SCHEDULES_PER_TAG; i++)
            out->schedule_ids[out->schedule_count++] =
                (uint32_t)cJSON_GetArrayItem(sched_arr, i)->valuedouble;
    }

    cJSON *type_item = cJSON_GetObjectItem(obj, "tag_type");
    out->tag_type = type_item ? (TagType)type_item->valueint : TAG_TYPE_RSSI;
}

// ─────────────────────────────────────────────────────────────────────────────
// Schedule serialization
// ─────────────────────────────────────────────────────────────────────────────

static cJSON *schedule_to_json(const Schedule *s) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "schedule_id",   (double)s->schedule_id);
    cJSON_AddNumberToObject(obj, "user_id",       (double)s->user_id);
    cJSON_AddStringToObject(obj, "schedule_name", s->schedule_name);
    cJSON_AddNumberToObject(obj, "start_hour",    s->start_time.hour);
    cJSON_AddNumberToObject(obj, "start_minute",  s->start_time.minute);
    cJSON_AddNumberToObject(obj, "end_hour",      s->end_time.hour);
    cJSON_AddNumberToObject(obj, "end_minute",    s->end_time.minute);
    cJSON_AddNumberToObject(obj, "repeat_days",   s->repeat_days);
    cJSON_AddBoolToObject  (obj, "is_active",     s->is_active);
    cJSON_AddNumberToObject(obj, "required_mask",  (double)s->required_mask);
    return obj;
}

static void json_to_schedule(const cJSON *obj, Schedule *out) {
    out->schedule_id       = (uint32_t)cJSON_GetObjectItem(obj, "schedule_id")->valuedouble;
    out->user_id           = (uint32_t)cJSON_GetObjectItem(obj, "user_id")->valuedouble;
    out->start_time.hour   = (uint16_t)cJSON_GetObjectItem(obj, "start_hour")->valueint;
    out->start_time.minute = (uint16_t)cJSON_GetObjectItem(obj, "start_minute")->valueint;
    out->end_time.hour     = (uint16_t)cJSON_GetObjectItem(obj, "end_hour")->valueint;
    out->end_time.minute   = (uint16_t)cJSON_GetObjectItem(obj, "end_minute")->valueint;
    out->repeat_days       = (DayMask) cJSON_GetObjectItem(obj, "repeat_days")->valueint;
    out->is_active         = cJSON_IsTrue(cJSON_GetObjectItem(obj, "is_active"));
    strncpy(out->schedule_name,
            cJSON_GetObjectItem(obj, "schedule_name")->valuestring,
            sizeof(out->schedule_name) - 1);

    cJSON *mask_item = cJSON_GetObjectItem(obj, "required_mask");
    out->required_mask = mask_item ? (uint32_t)mask_item->valuedouble : 0;
}
// ─────────────────────────────────────────────────────────────────────────────
// Hash Helper
// ─────────────────────────────────────────────────────────────────────────────

static uint32_t hash_mac(const uint8_t mac[6]) {
    uint32_t h = 2166136261u;   // FNV-1a offset basis
    
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;         // FNV prime
    }
    
    return h & (HASH_SIZE - 1); // mask to table size
}

static void hashtable_reinsert(Tag *t) {
    uint32_t idx = hash_mac(t->mac_address);
    for (int i = 0; i < HASH_SIZE; i++) {
        uint32_t slot = (idx + i) & (HASH_SIZE - 1);
        if (!s_store.mac_index.buckets[slot].occupied) {
            s_store.mac_index.buckets[slot] = *t;
            s_store.mac_index.buckets[slot].occupied = true;
            return;
        }
    }
}
// ─────────────────────────────────────────────────────────────────────────────
// Free-list — slot allocation via bitmask
// ─────────────────────────────────────────────────────────────────────────────

// Returns the index of the first free slot (0–31), or -1 if full.
// __builtin_ctz counts trailing zeros — finds the lowest clear bit in O(1).
static int get_next_available_id(void) {
    uint32_t free_slots = ~s_store.used_slots_mask;
    if (free_slots == 0) return -1;     // all 32 slots occupied
    return __builtin_ctz(free_slots);   // index of lowest 0 bit
}

static void slot_alloc(int slot, Tag *t) {
    s_store.tag_id_map[slot]    = t;
    s_store.used_slots_mask    |= (1u << slot);
}

static void slot_free(int slot) {
    s_store.tag_id_map[slot]    = NULL;
    s_store.used_slots_mask    &= ~(1u << slot);
}

static int get_next_schedule_slot(void) {
    uint32_t free_slots = ~s_store.used_schedules_mask;
    // Mask to only consider the first MAX_SCHEDULES bits
    free_slots &= (1u << MAX_SCHEDULES) - 1; 
    if (free_slots == 0) return -1;
    return __builtin_ctz(free_slots);
}

static void sched_slot_alloc(int slot, Schedule *s) {
    s_store.schedule_id_map[slot] = s;
    s_store.used_schedules_mask |= (1u << slot);
}

static void sched_slot_free(int slot) {
    s_store.schedule_id_map[slot] = NULL;
    s_store.used_schedules_mask &= ~(1u << slot);
}
// ─────────────────────────────────────────────────────────────────────────────
// Load helpers
// ─────────────────────────────────────────────────────────────────────────────

static esp_err_t load_users(void) {
    char path[128]; make_path(path, sizeof(path), "users.json");
    char *raw = read_file(path);
    if (!raw) { ESP_LOGI(TAG, "users.json not found — starting empty"); return ESP_OK; }

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) { ESP_LOGE(TAG, "users.json parse error"); return ESP_FAIL; }

    cJSON *arr = cJSON_GetObjectItem(root, "users");
    int n = arr ? cJSON_GetArraySize(arr) : 0;
    s_store.user_count = 0;

    for (int i = 0; i < n && i < MAX_USERS; i++) {
        json_to_user(cJSON_GetArrayItem(arr, i), &s_store.users[s_store.user_count]);
        uint32_t id = s_store.users[s_store.user_count].user_id;
        if (id >= s_next_user_id) s_next_user_id = id + 1;
        s_store.user_count++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d users", s_store.user_count);
    return ESP_OK;
}

static esp_err_t load_tags(void) {
    char path[128]; make_path(path, sizeof(path), "tags.json");
    char *raw = read_file(path);
    if (!raw) { ESP_LOGI(TAG, "tags.json not found — starting empty"); return ESP_OK; }

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) { ESP_LOGE(TAG, "tags.json parse error"); return ESP_FAIL; }

    cJSON *arr = cJSON_GetObjectItem(root, "tags");
    int n = arr ? cJSON_GetArraySize(arr) : 0;
    s_store.tag_count = 0;

    for (int i = 0; i < n; i++) {
        Tag tmp = {0};
        json_to_tag(cJSON_GetArrayItem(arr, i), &tmp);

        // tag_id is the slot index saved in JSON — validate it
        if (tmp.tag_id >= MAX_TAGS) {
            ESP_LOGW(TAG, "load_tags: tag_id %lu out of range, skipping",
                     (unsigned long)tmp.tag_id);
            continue;
        }
        if (s_store.used_slots_mask & (1u << tmp.tag_id)) {
            ESP_LOGW(TAG, "load_tags: slot %lu already occupied, skipping",
                     (unsigned long)tmp.tag_id);
            continue;
        }

        // Insert into hashtable
        uint32_t idx = hash_mac(tmp.mac_address);
        bool inserted = false;
        for (int j = 0; j < HASH_SIZE; j++) {
            uint32_t slot = (idx + j) & (HASH_SIZE - 1);
            if (!s_store.mac_index.buckets[slot].occupied) {
                s_store.mac_index.buckets[slot]          = tmp;
                s_store.mac_index.buckets[slot].occupied = true;
                /* Backwards-compat: old saves had no user_id field → assign
                 * to the first stored user so the tag isn't shown as Unknown */
                if (s_store.mac_index.buckets[slot].user_id == 0 &&
                    s_store.user_count > 0) {
                    s_store.mac_index.buckets[slot].user_id =
                        s_store.users[0].user_id;
                }
                slot_alloc((int)tmp.tag_id, &s_store.mac_index.buckets[slot]);
                s_store.tag_count++;
                inserted = true;
                break;
            }
        }
        if (!inserted)
            ESP_LOGE(TAG, "load_tags: hashtable full, could not insert tag %lu",
                     (unsigned long)tmp.tag_id);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d tags", s_store.tag_count);
    return ESP_OK;
}

static esp_err_t load_schedules(void) {
    char path[128]; make_path(path, sizeof(path), "schedules.json");
    char *raw = read_file(path);
    if (!raw) { ESP_LOGI(TAG, "schedules.json not found — starting empty"); return ESP_OK; }

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) { ESP_LOGE(TAG, "schedules.json parse error"); return ESP_FAIL; }

    cJSON *arr = cJSON_GetObjectItem(root, "schedules");
    int n = arr ? cJSON_GetArraySize(arr) : 0;
    s_store.schedule_count = 0;
    s_store.used_schedules_mask = 0;

    for (int i = 0; i < n && i < MAX_SCHEDULES; i++) {
        Schedule *s = &s_store.schedules[s_store.schedule_count];
        json_to_schedule(cJSON_GetArrayItem(arr, i), s);
        
        uint32_t id = s->schedule_id;
        if (id < MAX_SCHEDULES) {
            sched_slot_alloc(id, s);
            s_store.schedule_count++;
        } else {
            ESP_LOGE(TAG, "Loaded schedule ID %lu out of range", (unsigned long)id);
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d schedules", s_store.schedule_count);
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Save helpers
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t store_save_users(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    for (int i = 0; i < s_store.user_count; i++)
        cJSON_AddItemToArray(arr, user_to_json(&s_store.users[i]));
    cJSON_AddItemToObject(root, "users", arr);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char path[128]; make_path(path, sizeof(path), "users.json");
    esp_err_t ret = write_file(path, str);
    free(str);
    return ret;
}

esp_err_t store_save_tags(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    for (int i = 0; i < HASH_SIZE; i++) {
        if (s_store.mac_index.buckets[i].occupied)
            cJSON_AddItemToArray(arr, tag_to_json(&s_store.mac_index.buckets[i]));
    }
    cJSON_AddItemToObject(root, "tags", arr);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char path[128]; make_path(path, sizeof(path), "tags.json");
    esp_err_t ret = write_file(path, str);
    free(str);
    return ret;
}

esp_err_t store_save_schedules(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    for (int i = 0; i < s_store.schedule_count; i++)
        cJSON_AddItemToArray(arr, schedule_to_json(&s_store.schedules[i]));
    cJSON_AddItemToObject(root, "schedules", arr);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char path[128]; make_path(path, sizeof(path), "schedules.json");
    esp_err_t ret = write_file(path, str);
    free(str);
    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t store_init(const char *base_dir) {
    memset(&s_store, 0, sizeof(s_store));
    strncpy(s_base_dir, base_dir, sizeof(s_base_dir) - 1);

    esp_err_t r = ESP_OK;
    r |= load_users();
    r |= load_tags();
    r |= load_schedules();
    return r;
}

esp_err_t store_save_all(void) {
    esp_err_t r = ESP_OK;
    r |= store_save_users();
    r |= store_save_tags();
    r |= store_save_schedules();
    return r;
}

Store *store_get(void) { return &s_store; }

// ─────────────────────────────────────────────────────────────────────────────
// Users
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t store_user_add(const char *username, uint8_t loudness,
                          uint8_t brightness, User **out) {
    if (s_store.user_count >= MAX_USERS) {
        ESP_LOGE(TAG, "store_user_add: MAX_USERS reached"); return ESP_FAIL;
    }
    // Check for duplicate username
    for (int i = 0; i < s_store.user_count; i++) {
        if (strcmp(s_store.users[i].username, username) == 0) {
            ESP_LOGE(TAG, "store_user_add: username '%s' already exists", username);
            return ESP_FAIL;
        }
    }
    User *u = &s_store.users[s_store.user_count++];
    memset(u, 0, sizeof(*u));
    u->user_id          = s_next_user_id++;
    u->audio_loudness   = loudness;
    u->alert_brightness = brightness;
    strncpy(u->username, username, sizeof(u->username) - 1);
    if (out) *out = u;
    return store_save_users();
}

esp_err_t store_user_find_by_id(uint32_t user_id, User **out) {
    for (int i = 0; i < s_store.user_count; i++) {
        if (s_store.users[i].user_id == user_id) {
            if (out) *out = &s_store.users[i];
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t store_user_find_by_name(const char *username, User **out) {
    for (int i = 0; i < s_store.user_count; i++) {
        if (strcmp(s_store.users[i].username, username) == 0) {
            if (out) *out = &s_store.users[i];
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t store_user_update_settings(uint32_t user_id,
                                      uint8_t loudness, uint8_t brightness) {
    User *u;
    if (store_user_find_by_id(user_id, &u) != ESP_OK) return ESP_FAIL;
    u->audio_loudness   = loudness;
    u->alert_brightness = brightness;
    return store_save_users();
}

esp_err_t store_user_delete(uint32_t user_id) {
    for (int i = 0; i < s_store.user_count; i++) {
        if (s_store.users[i].user_id == user_id) {
            // Shift remaining users left
            memmove(&s_store.users[i], &s_store.users[i + 1],
                    (size_t)(s_store.user_count - i - 1) * sizeof(User));
            s_store.user_count--;
            // Also delete all schedules owned by this user
            for (int j = s_store.schedule_count - 1; j >= 0; j--) {
                if (s_store.schedules[j].user_id == user_id) {
                    store_schedule_delete(s_store.schedules[j].schedule_id);
                }
            }
            store_save_schedules();
            return store_save_users();
        }
    }
    return ESP_FAIL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tags
// ─────────────────────────────────────────────────────────────────────────────


esp_err_t store_tag_add(const uint8_t mac[6], const char *name, Tag **out, uint32_t user_id) {
    // Check for existing tag with same MAC first
    uint32_t idx = hash_mac(mac);
    for (int i = 0; i < HASH_SIZE; i++) {
    uint32_t slot = (idx + i) & (HASH_SIZE - 1);
    Tag *t = &s_store.mac_index.buckets[slot];
    if (t->occupied && memcmp(t->mac_address, mac, 6) == 0) {
        if (out) *out = t;
        return ESP_OK;          // already exists
    }
    if (!t->occupied) break;    // end of probe chain, not found
    }

    if (s_store.tag_count >= MAX_TAGS) {
        ESP_LOGE(TAG, "store_tag_add: MAX_TAGS reached");
        return ESP_FAIL;
    }

    // Allocate a free slot — this becomes the tag's permanent ID
    int new_id = get_next_available_id();
    if (new_id < 0) {
        ESP_LOGE(TAG, "store_tag_add: no free slots");
        return ESP_FAIL;
    }

     

    // Find empty hashtable bucket
    for (int i = 0; i < HASH_SIZE; i++) {
        uint32_t bucket = (idx + i) & (HASH_SIZE - 1);
        Tag *t = &s_store.mac_index.buckets[bucket];
        if (!t->occupied) {
            memset(t, 0, sizeof(*t));
            t->occupied       = true;   // ← mark as in use
            t->tag_id         = (uint32_t)new_id;
            t->schedule_count = 0;
            t->user_id = user_id;
            memcpy(t->mac_address, mac, 6);
            strncpy(t->tag_name, name, sizeof(t->tag_name) - 1);
            t->tag_type = (strncmp(name, "Tether Tag", 10) == 0)
                        ? TAG_TYPE_ACCEL : TAG_TYPE_RSSI;
            slot_alloc(new_id, t);
            s_store.tag_count++;
            if (out) *out = t;
            return store_save_tags();
        }
    }
    return ESP_FAIL;    // hashtable full (shouldn't happen with HASH_SIZE >= MAX_TAGS*2)
}

esp_err_t store_tag_find_by_id(uint32_t tag_id, Tag **out) {
    if (tag_id >= MAX_TAGS) return ESP_FAIL;
    Tag *t = s_store.tag_id_map[tag_id];
    if (!t) return ESP_FAIL;
    if (out) *out = t;
    return ESP_OK;
}

esp_err_t store_tag_find_by_mac(const uint8_t mac[6], Tag **out) {
    uint32_t idx = hash_mac(mac);
    for (int i = 0; i < HASH_SIZE; i++) {
        uint32_t slot = (idx + i) & (HASH_SIZE - 1);
        Tag *t = &s_store.mac_index.buckets[slot];
        if (!t->occupied)                          return ESP_FAIL;
        if (memcmp(t->mac_address, mac, 6) == 0) {
            if (out) *out = t;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t store_tag_delete(uint32_t tag_id) {
    if (tag_id >= MAX_TAGS) return ESP_FAIL;
    Tag *t = s_store.tag_id_map[tag_id];
    if (!t) return ESP_FAIL;

    // Remove this tag from every schedule it belongs to
    for (int j = 0; j < t->schedule_count; j++) {
        Schedule *s;
        if (store_schedule_find_by_id(t->schedule_ids[j], &s) != ESP_OK) continue;
        s->required_mask &= ~(1u << tag_id);
    }

    // Zero the hashtable bucket
    memset(t, 0, sizeof(Tag));

    // Rehash the probe chain following this bucket
    uint32_t bucket_idx = (uint32_t)(t - s_store.mac_index.buckets);
    uint32_t next = (bucket_idx + 1) & (HASH_SIZE - 1);
    while (s_store.mac_index.buckets[next].occupied) {
        Tag tmp = s_store.mac_index.buckets[next];
        memset(&s_store.mac_index.buckets[next], 0, sizeof(Tag));
        // Also clear id_map for this tag before reinserting
        s_store.tag_id_map[tmp.tag_id] = NULL;
        hashtable_reinsert(&tmp);
        // Rewire id_map after reinsert — find the new bucket
        uint32_t ridx = hash_mac(tmp.mac_address);
        for (int i = 0; i < HASH_SIZE; i++) {
            uint32_t rs = (ridx + i) & (HASH_SIZE - 1);
            Tag *rt = &s_store.mac_index.buckets[rs];
            if (rt->occupied && memcmp(rt->mac_address, tmp.mac_address, 6) == 0) {
                        s_store.tag_id_map[tmp.tag_id] = rt;
                        break;
                    }
        }
        next = (next + 1) & (HASH_SIZE - 1);
    }

    // Free the slot — makes this ID available for the next add
    slot_free((int)tag_id);
    s_store.tag_count--;
    store_save_schedules();
    return store_save_tags();
}

esp_err_t store_tag_update_settings(uint32_t tag_id, char *tag_name, uint32_t user_id) {
    Tag *t;
    if (store_tag_find_by_id(tag_id, &t) != ESP_OK) return ESP_FAIL;
    if (tag_name) strncpy(t->tag_name, tag_name, sizeof(t->tag_name) - 1);
    if (user_id != 0) t->user_id = user_id;
    return store_save_tags();
}
// ─────────────────────────────────────────────────────────────────────────────
// Schedules
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t store_schedule_add(uint32_t user_id, const char *name,
                              TimeOfDay start, TimeOfDay end,
                              DayMask days, Schedule **out) {
    if (s_store.schedule_count >= MAX_SCHEDULES) {
        ESP_LOGE(TAG, "store_schedule_add: MAX_SCHEDULES reached"); 
        return ESP_FAIL;
    }

    int new_id = get_next_schedule_slot();
    if (new_id < 0) return ESP_FAIL;

    Schedule *s = &s_store.schedules[s_store.schedule_count++];
    memset(s, 0, sizeof(*s));
    
    s->schedule_id = (uint32_t)new_id; // Slot is now the ID
    s->user_id     = user_id;
    s->start_time  = start;
    s->end_time    = end;
    s->repeat_days = days;
    s->is_active   = true;
    strncpy(s->schedule_name, name, sizeof(s->schedule_name) - 1);
    
    sched_slot_alloc(new_id, s);
    
    if (out) *out = s;
    return store_save_schedules();
}

esp_err_t store_schedule_find_by_id(uint32_t schedule_id, Schedule **out) {
    if (schedule_id >= MAX_SCHEDULES) return ESP_FAIL;
    
    Schedule *s = s_store.schedule_id_map[schedule_id];
    if (s && (s_store.used_schedules_mask & (1u << schedule_id))) {
        if (out) *out = s;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t store_schedule_add_tag(uint32_t schedule_id, uint32_t tag_id) {
    if (tag_id >= MAX_TAGS) return ESP_FAIL;
    Schedule *s;
    if (store_schedule_find_by_id(schedule_id, &s) != ESP_OK) return ESP_FAIL;

    s->required_mask |= (1u << tag_id);

    // Mirror on tag side
    Tag *t;
    if (store_tag_find_by_id(tag_id, &t) == ESP_OK) {
        int already = 0;
        for (int i = 0; i < t->schedule_count; i++)
            if (t->schedule_ids[i] == schedule_id) { already = 1; break; }
        if (!already && t->schedule_count < MAX_SCHEDULES_PER_TAG)
            t->schedule_ids[t->schedule_count++] = schedule_id;
    }

    store_save_tags();
    return store_save_schedules();
}

esp_err_t store_schedule_remove_tag(uint32_t schedule_id, uint32_t tag_id) {
    if (tag_id >= MAX_TAGS) return ESP_FAIL;
    Schedule *s;
    if (store_schedule_find_by_id(schedule_id, &s) != ESP_OK) return ESP_FAIL;

    s->required_mask &= ~(1u << tag_id);

    // Mirror removal on tag side
    Tag *t;
    if (store_tag_find_by_id(tag_id, &t) == ESP_OK) {
        for (int i = 0; i < t->schedule_count; i++) {
            if (t->schedule_ids[i] == schedule_id) {
                memmove(&t->schedule_ids[i], &t->schedule_ids[i + 1],
                        (size_t)(t->schedule_count - i - 1) * sizeof(uint32_t));
                t->schedule_count--;
                break;
            }
        }
    }

    store_save_tags();
    return store_save_schedules();
}

esp_err_t store_schedule_delete(uint32_t schedule_id) {
    for (int i = 0; i < s_store.schedule_count; i++) {
        if (s_store.schedules[i].schedule_id != schedule_id) continue;

        // Remove this schedule_id from every tag referenced by required_mask
        Schedule *s = &s_store.schedules[i];
        uint32_t mask = s->required_mask;
        while (mask) {
            int bit = __builtin_ctz(mask);
            mask &= mask - 1;
            Tag *t;
            if (store_tag_find_by_id((uint32_t)bit, &t) != ESP_OK) continue;
            for (int k = 0; k < t->schedule_count; k++) {
                if (t->schedule_ids[k] == schedule_id) {
                    memmove(&t->schedule_ids[k], &t->schedule_ids[k + 1],
                            (size_t)(t->schedule_count - k - 1) * sizeof(uint32_t));
                    t->schedule_count--;
                    break;
                }
            }
        }

        sched_slot_free((int)schedule_id);

        // Shift array and RE-MAP pointers
        int move_count = s_store.schedule_count - i - 1;
        if (move_count > 0) {
            memmove(&s_store.schedules[i], &s_store.schedules[i + 1],
                    (size_t)move_count * sizeof(Schedule));
            for (int j = i; j < s_store.schedule_count - 1; j++) {
                uint32_t active_id = s_store.schedules[j].schedule_id;
                s_store.schedule_id_map[active_id] = &s_store.schedules[j];
            }
        }

        s_store.schedule_count--;
        store_save_tags();
        return store_save_schedules();
    }
    return ESP_FAIL;
}

esp_err_t store_schedule_get_active(uint16_t hour, uint16_t minute,
                                     DayMask day,
                                     Schedule **out_ptrs, int max, int *count) {
    *count = 0;
    uint16_t now_mins = hour * 60u + minute;
    for (int i = 0; i < s_store.schedule_count && *count < max; i++) {
        Schedule *s = &s_store.schedules[i];
        if (!s->is_active) continue;
        if (!(s->repeat_days & day)) continue;
        uint16_t start_mins = s->start_time.hour * 60u + s->start_time.minute;
        uint16_t end_mins   = s->end_time.hour   * 60u + s->end_time.minute;
        if (now_mins >= start_mins && now_mins < end_mins)
            out_ptrs[(*count)++] = s;
    }
    return ESP_OK;
}

esp_err_t store_schedule_update_settings(uint32_t schedule_id,char *schedule_name, TimeOfDay start_time, TimeOfDay end_time, DayMask days) {
    Schedule *s;
    if (store_schedule_find_by_id(schedule_id, &s) != ESP_OK) return ESP_FAIL;
    strncpy(s->schedule_name, schedule_name, sizeof(s->schedule_name) - 1);
    s->start_time = start_time;
    s->end_time = end_time;
    s->repeat_days = days;
    return store_save_schedules();
}
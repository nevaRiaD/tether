#ifdef HOST_BUILD
  #include "esp_stub.h"
#else
  #include "esp_err.h"
  #include "esp_log.h"
#endif

#include "store.h"
#include "tag.h"
#include "models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static const char *TAG_LOG = "TAG";

esp_err_t tag_algorithm(const uint8_t entries[][6], int mac_count,
                         UserDetectionResult *out_results, int max_results,
                         int *out_count)
{
    *out_count = 0;

    /* ── Phase 1: identify which users are present ─────────────────────── */
    uint32_t detected_user_ids[MAX_USERS];
    uint32_t detected_tag_bitmap = 0;       /* bitmask of detected tag_ids */
    int      user_count = 0;

    for (int i = 0; i < mac_count; i++) {
        Tag *t = NULL;
        if (store_tag_find_by_mac(entries[i], &t) != ESP_OK)
            continue;

        detected_tag_bitmap |= (1u << t->tag_id);

        /* Check if we already recorded this user */
        bool seen = false;
        for (int u = 0; u < user_count; u++) {
            if (detected_user_ids[u] == t->user_id) {
                seen = true;
                break;
            }
        }
        if (!seen && user_count < MAX_USERS) {
            detected_user_ids[user_count++] = t->user_id;
        }
    }

    /* ── Phase 2: for each user, find their missing tags ───────────────── */
    Store *store = store_get();

    for (int u = 0; u < user_count && *out_count < max_results; u++) {
        UserDetectionResult *res = &out_results[*out_count];
        res->user_id = detected_user_ids[u];
        res->missing_count = 0;

        /* Scan all tag slots for tags owned by this user */
        for (int slot = 0; slot < HASH_SIZE; slot++) {
            Tag *t = &store->mac_index.buckets[slot];
            if (!t->occupied) continue;
            if (t->user_id != detected_user_ids[u]) continue;

            /* This tag belongs to the user — was it detected? */
            if (!(detected_tag_bitmap & (1u << t->tag_id))) {
                if (res->missing_count < MAX_TAGS) {
                    res->missing[res->missing_count++] = t;
                }
            }
        }

        ESP_LOGI(TAG_LOG, "User %" PRIu32 " detected, %d tag(s) missing",
                 res->user_id, res->missing_count);
        (*out_count)++;
    }

    return ESP_OK;
}

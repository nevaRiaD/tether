#ifndef TAG_H
#define TAG_H

#include "esp_err.h"
#include "models.h"

/**
 * Result of the tag detection algorithm for a single identified user.
 */
typedef struct {
    uint32_t user_id;
    Tag     *missing[MAX_TAGS];     /* tags this user owns but weren't detected */
    int      missing_count;
} UserDetectionResult;

/**
 * @brief  Identify which users are present and what tags they're missing.
 *
 * For each MAC in the scan report, look up the owning user_id. Then for each
 * identified user, query the store for all their tags — any tag not in the
 * detected set is reported as missing.
 *
 * @param entries       MAC addresses from the C6 scan report (close + moving).
 * @param mac_count     Number of entries.
 * @param out_results   Caller-allocated array of UserDetectionResult.
 * @param max_results   Size of out_results array.
 * @param out_count     Number of distinct users detected (written on return).
 * @return ESP_OK on success.
 */
esp_err_t tag_algorithm(const uint8_t entries[][6], int mac_count,
                         UserDetectionResult *out_results, int max_results,
                         int *out_count);

#endif // TAG_H
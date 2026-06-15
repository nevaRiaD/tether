#include "TEST_schedule.h"

// ── Shared state set by setup, used by every test ────────────────────────────
 
static Tag     *s_t0 = NULL;   // in schedule
static Tag     *s_t1 = NULL;   // in schedule
static Tag     *s_t2 = NULL;   // NOT in schedule
static Schedule *s_sched = NULL;

#define TEST_TAG "SCHED_TEST"

// ─────────────────────────────────────────────────────────────────────────────


static void setup_schedule_test_data(void)
{
    uint8_t mac0[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t mac1[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint8_t mac2[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
 
    store_tag_add(mac0, "Tag0", &s_t0, 0);
    store_tag_add(mac1, "Tag1", &s_t1, 0);
    store_tag_add(mac2, "Tag2", &s_t2, 0);
 
    TimeOfDay start = { .hour = 8,  .minute = 0 };
    TimeOfDay end   = { .hour = 10, .minute = 0 };
 
    ClockNow now;
    clock_set_manual(2026, 4, 20, 9, 0, 0);
    clock_get_now(&now);
 
    store_schedule_add(0, "Morning", start, end, now.day, &s_sched);
 
    store_schedule_add_tag(s_sched->schedule_id, s_t0->tag_id);
    store_schedule_add_tag(s_sched->schedule_id, s_t1->tag_id);
 
    ESP_LOGI(TEST_TAG,
             "Setup: tag_ids=(%lu,%lu,%lu) schedule required_mask=0x%08lx",
             (unsigned long)s_t0->tag_id,
             (unsigned long)s_t1->tag_id,
             (unsigned long)s_t2->tag_id,
             (unsigned long)s_sched->required_mask);
}


// ─────────────────────────────────────────────────────────────────────────────
 
static void check_tag_mask(const Tag *t, uint32_t expected_mask)
{
    if (t == NULL) {
        ESP_LOGE(TEST_TAG, "FAIL: NULL tag pointer");
        return;
    }
 
    ESP_LOGI(TEST_TAG,
             "Tag %lu mask = 0x%08lx, expected = 0x%08lx",
             (unsigned long)t->tag_id,
             (unsigned long)t->tag_collection_mask,
             (unsigned long)expected_mask);
 
    if (t->tag_collection_mask != expected_mask) {
        ESP_LOGE(TEST_TAG, "FAIL: Tag %lu mask mismatch", (unsigned long)t->tag_id);
    } else {
        ESP_LOGI(TEST_TAG, "PASS: Tag %lu mask correct", (unsigned long)t->tag_id);
    }
}
 
// ─────────────────────────────────────────────────────────────────────────────
 
static void test_schedule_active(void)
{
    ESP_LOGI(TEST_TAG, "==== Test 1: active schedule ====");
 
    clock_set_manual(2026, 4, 20, 9, 0, 0);
 
    ClockNow now;
    if (clock_get_now(&now) != ESP_OK) {
        ESP_LOGE(TEST_TAG, "FAIL: clock_get_now failed");
        return;
    }
 
    ESP_LOGI(TEST_TAG, "Clock: hour=%d minute=%d day=0x%02x",
             now.hour, now.minute, now.day);
 
    if (schedule_algorithm(now.hour, now.minute, now.day) != ESP_OK) {
        ESP_LOGE(TEST_TAG, "FAIL: schedule_algorithm failed");
        return;
    }
 
    /*
        required_mask = (1 << t0->tag_id) | (1 << t1->tag_id)
 
        t0 and t1 are in the schedule → their masks get ORed with required_mask.
        Because their own bit is already in required_mask:
            t0->tag_collection_mask = required_mask
            t1->tag_collection_mask = required_mask
 
        t2 is not in any schedule → reset only:
            t2->tag_collection_mask = (1 << t2->tag_id)
    */
 
    uint32_t required = s_sched->required_mask;
 
    check_tag_mask(s_t0, required);
    check_tag_mask(s_t1, required);
    check_tag_mask(s_t2, (1u << s_t2->tag_id));
}
 
static void test_schedule_inactive_reset(void)
{
    ESP_LOGI(TEST_TAG, "==== Test 2: inactive schedule reset ====");
 
    // 11:00 AM — outside the 08:00–10:00 window
    clock_set_manual(2026, 4, 20, 11, 0, 0);
 
    ClockNow now;
    if (clock_get_now(&now) != ESP_OK) {
        ESP_LOGE(TEST_TAG, "FAIL: clock_get_now failed");
        return;
    }
 
    ESP_LOGI(TEST_TAG, "Clock: hour=%d minute=%d day=0x%02x",
             now.hour, now.minute, now.day);
 
    if (schedule_algorithm(now.hour, now.minute, now.day) != ESP_OK) {
        ESP_LOGE(TEST_TAG, "FAIL: schedule_algorithm failed");
        return;
    }
 
    // No active schedules — every tag resets to (1 << tag_id)
    check_tag_mask(s_t0, (1u << s_t0->tag_id));
    check_tag_mask(s_t1, (1u << s_t1->tag_id));
    check_tag_mask(s_t2, (1u << s_t2->tag_id));
}
 
static void test_tag_algorithm_missing_one(void)
{
    ESP_LOGI(TEST_TAG, "==== Test 3: tag algorithm missing one ====");
 
    clock_set_manual(2026, 4, 20, 9, 0, 0);
 
    ClockNow now;
    clock_get_now(&now);
    schedule_algorithm(now.hour, now.minute, now.day);
 
    // Only t0 is detected; t1 should be reported missing
    uint8_t detected_macs[1][6];
    memcpy(detected_macs[0], s_t0->mac_address, 6);
 
    Tag *missing[8];
    int missing_count = 0;
 
    tag_algorithm(detected_macs, 1, missing, 8, &missing_count);
 
    if (missing_count != 1) {
        ESP_LOGE(TEST_TAG, "FAIL: expected 1 missing tag, got %d", missing_count);
        return;
    }
 
    if (missing[0]->tag_id != s_t1->tag_id) {
        ESP_LOGE(TEST_TAG, "FAIL: expected missing Tag %lu, got Tag %lu",
                 (unsigned long)s_t1->tag_id,
                 (unsigned long)missing[0]->tag_id);
        return;
    }
 
    ESP_LOGI(TEST_TAG, "PASS: Missing Tag %lu detected correctly",
             (unsigned long)s_t1->tag_id);
}
 
static void test_tag_algorithm_all_detected(void)
{
    ESP_LOGI(TEST_TAG, "==== Test 4: tag algorithm all detected ====");
 
    clock_set_manual(2026, 4, 20, 9, 0, 0);
 
    ClockNow now;
    clock_get_now(&now);
    schedule_algorithm(now.hour, now.minute, now.day);
 
    uint8_t detected_macs[2][6];
    memcpy(detected_macs[0], s_t0->mac_address, 6);
    memcpy(detected_macs[1], s_t1->mac_address, 6);
 
    Tag *missing[8];
    int missing_count = 0;
 
    tag_algorithm(detected_macs, 2, missing, 8, &missing_count);
 
    if (missing_count == 0) {
        ESP_LOGI(TEST_TAG, "PASS: All required tags detected");
    } else {
        ESP_LOGE(TEST_TAG, "FAIL: expected 0 missing tags, got %d", missing_count);
    }
}
 
// ─────────────────────────────────────────────────────────────────────────────
 
void run_schedule_tests(void)
{
    ESP_LOGI(TEST_TAG, "========== Running schedule tests ==========");
 
    setup_schedule_test_data();
 
    test_schedule_active();
    test_schedule_inactive_reset();
    test_tag_algorithm_missing_one();
    test_tag_algorithm_all_detected();
 
    ESP_LOGI(TEST_TAG, "========== Schedule tests complete ==========");
}

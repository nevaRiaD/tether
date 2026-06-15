#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "store.h"
#include "schedule.h"
#include "tag.h"
#include "models.h"
#include "tag.h"
#include "storage_test.h"

static const char *TAG = "test_suite";

// ─────────────────────────────────────────────────────────────────────────────
// Test Harness
// ─────────────────────────────────────────────────────────────────────────────

static int s_pass = 0;
static int s_fail = 0;

#define TEST(name) ESP_LOGI(TAG, "\n--- TEST: %s ---", name)

#define ASSERT_OK(expr) \
    do { \
        esp_err_t _r = (expr); \
        if (_r == ESP_OK) { s_pass++; ESP_LOGI(TAG, "[PASS] %s", #expr); } \
        else { ESP_LOGE(TAG, "[FAIL] %s (err: 0x%x)", #expr, _r); s_fail++; return; } \
    } while(0)

#define ASSERT_FAIL(expr) \
    do { \
        if ((expr) != ESP_OK) { s_pass++; ESP_LOGI(TAG, "[PASS] Expected fail: %s", #expr); } \
        else { ESP_LOGE(TAG, "[FAIL] %s (expected fail but got OK)", #expr); s_fail++; return; } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) == (b)) { s_pass++; ESP_LOGI(TAG, "[PASS] %s == %d", #a, (int)b); } \
        else { ESP_LOGE(TAG, "[FAIL] %s=%d, expected %d", #a, (int)(a), (int)(b)); s_fail++; return; } \
    } while(0)

#define ASSERT_STR(a, b) \
    do { \
        if (a && b && strcmp((a),(b)) == 0) { s_pass++; ESP_LOGI(TAG, "[PASS] %s == \"%s\"", #a, b); } \
        else { ESP_LOGE(TAG, "[FAIL] %s=\"%s\", expected \"%s\"", #a, a ? a : "NULL", b); s_fail++; return; } \
    } while(0)

#define ASSERT_NOT_NULL(p) \
    do { \
        if ((p) != NULL) { s_pass++; } \
        else { ESP_LOGE(TAG, "[FAIL] %s is NULL", #p); s_fail++; return; } \
    } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void cleanup_store(void) {
    remove("/littlefs/users.json");
    remove("/littlefs/tags.json");
    remove("/littlefs/schedules.json");
    store_init("/littlefs");
}

static const uint8_t MAC_WALLET[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
static const uint8_t MAC_KEYS[6]   = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t MAC_BAG[6]    = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

// ─────────────────────────────────────────────────────────────────────────────
// Logic Tests
// ─────────────────────────────────────────────────────────────────────────────

static void test_users_and_tags(void) {
    TEST("User/Tag Core Logic");
    cleanup_store();

    User *alice = NULL;
    ASSERT_OK(store_user_add("alice", 75, 90, &alice));
    ASSERT_STR(alice->username, "alice");

    // Test find mechanics
    User *found = NULL;
    ASSERT_OK(store_user_find_by_name("alice", &found));
    ASSERT_EQ(found->user_id, alice->user_id);

    // Test tag duplication protection
    Tag *t1 = NULL, *t2 = NULL;
    ASSERT_OK(store_tag_add(MAC_WALLET, "Wallet", &t1, 1));
    ASSERT_OK(store_tag_add(MAC_WALLET, "Clone", &t2, 1));
    ASSERT_EQ(t1->tag_id, t2->tag_id); 
    ASSERT_STR(t2->tag_name, "Wallet"); // Name should not change on duplicate add
}

static void test_bidirectional_schedules(void) {
    TEST("Bidirectional Schedule Relationships");
    cleanup_store();

    User *u = NULL;
    Tag *wallet = NULL, *keys = NULL;
    Schedule *morning = NULL;

    store_user_add("alice", 50, 50, &u);
    store_tag_add(MAC_WALLET, "Wallet", &wallet, 1);
    store_tag_add(MAC_KEYS, "Keys", &keys, 1);

    ASSERT_OK(store_schedule_add(u->user_id, "Morning", (TimeOfDay){8,0}, (TimeOfDay){9,0}, DAYS_WEEKDAY, &morning));

    // Link Tags
    ASSERT_OK(store_schedule_add_tag(morning->schedule_id, wallet->tag_id));
    ASSERT_OK(store_schedule_add_tag(morning->schedule_id, keys->tag_id));

    // Verify Schedule Side (Masks)
    int tag_count = 0;
    uint32_t mask = morning->required_mask;
    while(mask) { if(mask & 1) tag_count++; mask >>= 1; }
    ASSERT_EQ(tag_count, 2);

    // Verify Tag Side (Back-references)
    ASSERT_EQ(wallet->schedule_count, 1);
    ASSERT_EQ(wallet->schedule_ids[0], morning->schedule_id);

    // Test Active Window Query
    Schedule *active[5];
    int count = 0;
    ASSERT_OK(store_schedule_get_active(8, 30, DAY_MON, active, 5, &count));
    ASSERT_EQ(count, 1);
    ASSERT_STR(active[0]->schedule_name, "Morning");

    // Test removal logic
    ASSERT_OK(store_schedule_remove_tag(morning->schedule_id, wallet->tag_id));
    ASSERT_EQ(wallet->schedule_count, 0);
}

static void test_persistence_integrity(void) {
    TEST("Persistence Integrity");
    cleanup_store();

    User *u = NULL;
    store_user_add("bob", 10, 20, &u);
    store_save_all();

    // Reload
    ASSERT_OK(store_init("/littlefs"));
    User *reloaded = NULL;
    ASSERT_OK(store_user_find_by_name("bob", &reloaded));
    ASSERT_EQ(reloaded->audio_loudness, 10);
    ASSERT_EQ(reloaded->alert_brightness, 20);
}

static void test_cascade_destruction(void) {
    TEST("Cascade Delete Robustness");
    cleanup_store();

    User *u = NULL;
    Tag *t = NULL;
    Schedule *s = NULL;

    store_user_add("carol", 70, 70, &u);
    store_tag_add(MAC_BAG, "Bag", &t, 1);
    store_schedule_add(u->user_id, "Work", (TimeOfDay){9,0}, (TimeOfDay){17,0}, DAYS_WEEKDAY, &s);
    store_schedule_add_tag(s->schedule_id, t->tag_id);

    // Deleting tag should clear schedule mask
    ASSERT_OK(store_tag_delete(t->tag_id));
    ASSERT_EQ(s->required_mask, 0);

    // Deleting user should wipe schedule entirely
    ASSERT_OK(store_user_delete(u->user_id));
    ASSERT_EQ(store_get()->schedule_count, 0);
}

static void test_tag_algorithm_scenarios(void) {
    TEST("Algorithm: Partial & Full Detection");
    cleanup_store();

    // 1. Setup Data
    User *alice = NULL;
    Tag *wallet = NULL, *keys = NULL;
    Schedule *work = NULL;

    store_user_add("alice", 50, 50, &alice);
    store_tag_add(MAC_WALLET, "Wallet", &wallet, 1); // tag_id likely 0
    store_tag_add(MAC_KEYS,   "Keys",   &keys,   1); // tag_id likely 1

    // 2. Create a schedule that REQUIRES both Wallet (bit 0) and Keys (bit 1)
    // required_mask = (1 << 0) | (1 << 1) = 0x03
    ASSERT_OK(store_schedule_add(alice->user_id, "Work", 
                                (TimeOfDay){9,0}, (TimeOfDay){17,0}, 
                                DAYS_ALL, &work));
    
    ASSERT_OK(store_schedule_add_tag(work->schedule_id, wallet->tag_id));
    ASSERT_OK(store_schedule_add_tag(work->schedule_id, keys->tag_id));

    // 3. RUN THE SCHEDULE ALGORITHM (This prepares the tag_collection_masks)
    // We simulate it is 10:00 AM on a Monday
    ASSERT_OK(schedule_algorithm(10, 0, DAY_MON));

    // --- SCENARIO A: BOTH PRESENT ---
    uint8_t scanned_all[2][6];
    memcpy(scanned_all[0], MAC_WALLET, 6);
    memcpy(scanned_all[1], MAC_KEYS, 6);

    Tag *missing[MAX_TAGS];
    int missing_cnt = 0;
    
    ASSERT_OK(tag_algorithm(scanned_all, 2, missing, MAX_TAGS, &missing_cnt));
    ASSERT_EQ(missing_cnt, 0);

    // --- SCENARIO B: WALLET MISSING (Only Keys Scanned) ---
    uint8_t scanned_keys_only[1][6];
    memcpy(scanned_keys_only[0], MAC_KEYS, 6);
    
    // Now, because Keys are scanned, and schedule_algorithm set Keys->tag_collection_mask to 0x03,
    // the tag_algorithm will know that Wallet is missing.
    ASSERT_OK(tag_algorithm(scanned_keys_only, 1, missing, MAX_TAGS, &missing_cnt));
    
    ASSERT_EQ(missing_cnt, 1);
    ASSERT_STR(missing[0]->tag_name, "Wallet");
}

// ─────────────────────────────────────────────────────────────────────────────
// Runner
// ─────────────────────────────────────────────────────────────────────────────

void storage_test(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
    };

    if (esp_vfs_littlefs_register(&conf) != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed");
        return;
    }

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "   TETHER STORAGE & ALGO ROBUST TEST        ");
    ESP_LOGI(TAG, "============================================");

    test_users_and_tags();
    test_bidirectional_schedules();
    test_persistence_integrity();
    test_cascade_destruction();
    test_tag_algorithm_scenarios();

    ESP_LOGI(TAG, "\n============================================");
    if (s_fail == 0) {
        ESP_LOGI(TAG, "  OVERALL RESULT: PASS (%d assertions)", s_pass);
    } else {
        ESP_LOGE(TAG, "  OVERALL RESULT: FAIL (%d fails)", s_fail);
    }
    ESP_LOGI(TAG, "============================================");
}
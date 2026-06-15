#include "schedule.h"
#include "clock.h"
#include "tether_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCHED_TAG "SCHED"

static void schedule_task(void *arg)
{
    for (;;) {
        // Delay first so the initial evaluation runs after store_init has
        // loaded the store (the task is started before LittleFS is mounted).
        vTaskDelay(pdMS_TO_TICKS(SCHEDULE_TASK_DELAY));

        ClockNow now;
        if (clock_get_now(&now) != ESP_OK) {
            ESP_LOGE(SCHED_TAG, "clock_get_now failed");
        } else if (schedule_algorithm(now.hour, now.minute, now.day) != ESP_OK) {
            ESP_LOGE(SCHED_TAG, "schedule_algorithm failed");
        }
    }
}

void schedule_task_start(void)
{
    xTaskCreate(schedule_task, "schedule", 4096, NULL, 5, NULL);
}

esp_err_t schedule_algorithm(uint16_t hour, uint16_t minute, DayMask day)
{
    // 1. Reset every tag's collection mask to just its own bit
    for (int i = 0; i < MAX_TAGS; i++) {
        Tag *t = NULL;
        if (store_tag_find_by_id(i, &t) == ESP_OK) {
            t->tag_collection_mask = (1u << t->tag_id);
        }
    }

    // 2. Collect active schedules
    Schedule *active[MAX_SCHEDULES];
    int active_count = 0;

    esp_err_t err = store_schedule_get_active(
        hour, minute, day,
        active, MAX_SCHEDULES, &active_count
    );
    if (err != ESP_OK) return err;

#ifdef SCHEDULE_LOGS_MODE
    ESP_LOGI(SCHED_TAG, "Active schedules: %d", active_count);
#endif
    // 3. For each active schedule, OR its required_mask into every member tag
    for (int i = 0; i < active_count; i++) {
        Schedule *s = active[i];

#ifdef SCHEDULE_LOGS_MODE
        if (s->required_mask == 0) {
            ESP_LOGW(SCHED_TAG, "Schedule %lu has empty required_mask",
                     (unsigned long)s->schedule_id);
            continue;
        }

        ESP_LOGI(SCHED_TAG, "Schedule %lu required_mask=0x%08lx",
                 (unsigned long)s->schedule_id,
                 (unsigned long)s->required_mask);
#else // NO LOGS
        if (s->required_mask == 0) {
            continue;
        }
#endif
        // Iterate each set bit in required_mask
        uint32_t mask = s->required_mask;
        while (mask) {
            int bit = __builtin_ctz(mask); // index of lowest set bit
            mask &= mask - 1;             // clear it

            Tag *t = NULL;
            if (store_tag_find_by_id((uint32_t)bit, &t) != ESP_OK) continue;

#ifdef SCHEDULE_LOGS_MODE
            ESP_LOGI(SCHED_TAG, "Applying mask 0x%08lx to tag %d",
                     (unsigned long)s->required_mask, bit);
#endif
            t->tag_collection_mask |= s->required_mask;
        }
    }

    return ESP_OK;
}
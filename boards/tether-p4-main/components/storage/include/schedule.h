#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "models.h"
#include "store.h"

esp_err_t schedule_algorithm(uint16_t hour, uint16_t minute, DayMask day);

// Spawn a background task that re-evaluates schedules against the current
// clock time every 30 s. Call once after store_init.
void schedule_task_start(void);
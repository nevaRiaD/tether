#ifndef INCLUDE_CLOCK_H
#define INCLUDE_CLOCK_H

#include <stdint.h>
#include "esp_err.h"
#include "models.h"

typedef struct {
    uint16_t hour;
    uint16_t minute;
    DayMask day;
} ClockNow;

esp_err_t clock_set_manual(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second
);

esp_err_t clock_get_now(ClockNow *out);

#endif // INCLUDE_CLOCK_H
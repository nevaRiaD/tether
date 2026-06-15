#include "clock.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>
#include "tether_config.h"

#define CLOCK_TAG "CLOCK"

esp_err_t clock_set_manual(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second
)
{
    struct tm timeinfo = {0};

    timeinfo.tm_year  = year - 1900;
    timeinfo.tm_mon   = month - 1;
    timeinfo.tm_mday  = day;
    timeinfo.tm_hour  = hour;
    timeinfo.tm_min   = minute;
    timeinfo.tm_sec   = second;
    timeinfo.tm_isdst = -1;  /* let mktime determine DST from TZ env */

    time_t timestamp = mktime(&timeinfo);

    if (timestamp == -1){
        return ESP_FAIL;
    }

    
    struct timeval now = {
        .tv_sec = timestamp,
        .tv_usec = 0
    };

    esp_err_t err = settimeofday(&now, NULL);

    if (err == ESP_OK){
        ESP_LOGI(CLOCK_TAG, "Clock manually set");
    }

    return err;
}



static DayMask tm_wday_to_daymask(int tm_wday)
{
    // tm_wday: 0 = Sunday, 1 = Monday, ..., 6 = Saturday
    switch (tm_wday) {
        case 0: return DAY_SUN;
        case 1: return DAY_MON;
        case 2: return DAY_TUE;
        case 3: return DAY_WED;
        case 4: return DAY_THU;
        case 5: return DAY_FRI;
        case 6: return DAY_SAT;
        default: return 0;
    }
}


esp_err_t clock_get_now(ClockNow *out)
{
    if(out == NULL){
        return ESP_ERR_INVALID_ARG;
    }

    time_t now;
    struct tm timeinfo;

    time(&now);

    if(localtime_r(&now,&timeinfo) == NULL){
        return ESP_FAIL;
    }

    out->hour   = timeinfo.tm_hour;
    out->minute = timeinfo.tm_min;
    out->day    = tm_wday_to_daymask(timeinfo.tm_wday);
#ifdef SCHEDULE_LOGS_MODE
    ESP_LOGI(CLOCK_TAG, "Now: %02d:%02d, day mask: 0x%02x",
    out->hour, out->minute, out->day);
#endif
    return ESP_OK;
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "screen_manager.h"
#include "screens/home.h"
#include "store.h"
#include "models.h"
#include "theme.h"
#include "sdio_master.h"
#include "sdio_event.h"
#include "tether_ble.h"
#include "vl53l1_platform.h"
#include "tether_ble_console.h"
#include "schedule.h"
#include "audio.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "tether";

static void time_save_cb(lv_timer_t *tmr)
{
    (void)tmr;
    nvs_handle_t h;
    if (nvs_open("tether", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, "epoch", (int64_t)time(NULL));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void time_init(void)
{
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();

    nvs_handle_t h;
    int64_t saved = 0;
    if (nvs_open("tether", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i64(h, "epoch", &saved);
        nvs_close(h);
    }

    time_t base;
    if (saved > 0) {
        base = (time_t)saved;
    } else {
        static const char *months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };
        char mon_str[4];
        int day, year, hour, min, sec;
        sscanf(__DATE__, "%3s %d %d", mon_str, &day, &year);
        sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);
        int mon = 0;
        for (int i = 0; i < 12; i++) {
            if (__builtin_strcmp(months[i], mon_str) == 0) { mon = i; break; }
        }
        struct tm t = {
            .tm_year  = year - 1900,
            .tm_mon   = mon,
            .tm_mday  = day,
            .tm_hour  = hour,
            .tm_min   = min,
            .tm_sec   = sec,
            .tm_isdst = -1,
        };
        base = mktime(&t);
    }

    struct timeval tv = { .tv_sec = base, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void init_store(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = "/littlefs",
        .partition_label        = "littlefs",
        .format_if_mount_failed = true,
    };
    if (esp_vfs_littlefs_register(&conf) != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed");
        return;
    }

    store_init("/littlefs");

    Store *st = store_get();
    if (st->user_count == 0) {
        store_user_add("Umar", 70, 100, NULL);
        store_save_users();
        ESP_LOGI(TAG, "Created default user");
    }

    schedule_task_start();  /* re-evaluate schedules vs clock every 30 s */
}

void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    time_init();
    theme_init();
    init_store();
	audio_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg  = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size    = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer  = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
            .sw_rotate   = false,
        },
    };

    lv_display_t *display = bsp_display_start_with_config(&cfg);
    if (display == NULL) {
        ESP_LOGE(TAG, "Failed to start display");
        return;
    }

    bsp_display_backlight_on();
    Store *st = store_get();
    int brightness = (st->user_count > 0) ? st->users[0].alert_brightness : 100;
    bsp_display_brightness_set(brightness < 10 ? 10 : brightness);

    bsp_display_lock(0);
    screen_manager_load(SCREEN_BOOT);
    home_set_status(TETHER_STATUS_UNKNOWN);
    lv_timer_create(time_save_cb, 60000, NULL);
    bsp_display_unlock();

    tether_ble_console_init();
    VL531LX_Setup();

    essl_handle_t sdio_handle;
    if (sdio_master_init(&sdio_handle) != ESP_OK) {
        ESP_LOGW(TAG, "SDIO init failed; running without C6 link");
    } else if (sdio_master_reset(sdio_handle) != ESP_OK) {
        ESP_LOGW(TAG, "SDIO reset failed");
    } else if (tether_ble_init(sdio_handle) != ESP_OK) {
        ESP_LOGW(TAG, "BLE bridge init failed");
    } else {
        sdio_event_start(sdio_handle);
    }

    ESP_LOGI(TAG, "UI running");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#include "audio.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "bsp_board_extra.h"

static const char *TAG = "audio";

void audio_init(void)
{
    ESP_LOGI(TAG, "init");
    esp_vfs_littlefs_conf_t audio_conf = {
        .base_path = "/audio",
        .partition_label = "audio",
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&audio_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_err_t err;
    err = bsp_extra_codec_init();
    ESP_LOGI(TAG, "codec_init: %s", esp_err_to_name(err));
    err = bsp_extra_player_init();
    ESP_LOGI(TAG, "player_init: %s", esp_err_to_name(err));
}

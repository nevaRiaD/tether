#include "nvs_flash.h"
#include "ble_tether.h"
#include "ble_pair_console.h"
#include "tether_sdio_slave.h"
#include "tether_scan.h"

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ble_tether_init();
    // tether_sdio_init();
    // ble_pair_console_init();
    moving_tag_t tags[MAX_CONNECTED_DEVICES];
    while (1){
        int count = tether_scan(tags, MAX_CONNECTED_DEVICES);
        printf("Moving tags info:\n");
        for (int i = 0; i < count; i++) {
            printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X, Name: %s\n", tags[i].mac[5], tags[i].mac[4], tags[i].mac[3], tags[i].mac[2], tags[i].mac[1], tags[i].mac[0], tags[i].name);
        }
    }
}

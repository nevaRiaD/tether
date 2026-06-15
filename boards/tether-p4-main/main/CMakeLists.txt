idf_component_register(
    SRCS "tether-project.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        storage
        ui
        audio
        sdio-master
        tether-ble
        tether-ble-console
        driver
        log
        esp_timer
        vl53l1x
    PRIV_REQUIRES
        waveshare__esp32_p4_wifi6_touch_lcd_7b
)


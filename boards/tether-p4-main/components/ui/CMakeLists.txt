idf_component_register(
    SRCS
        "src/theme.c"
        "src/screen_manager.c"
        "src/sdio_event.c"
        "src/images/allgood.c"
        "src/images/missing.c"
        "src/images/settings.c"
        "src/images/warning.c"
        "src/images/boot.c"

        "src/screens/home.c"
        "src/screens/boot.c"
        "src/screens/settings.c"
        "src/screens/tag_pair.c"
        "src/screens/user_mgmt.c"
        "src/screens/set_time.c"
        "src/screens/schedule_edit.c"
        "src/screens/tag_edit.c"

    INCLUDE_DIRS
        "include"
        "src"
        "src/screens"
        "src/images"

    REQUIRES
        storage
        lvgl
        nvs_flash
        log
        esp_timer
        driver
        sdio-master
        tether-ble
        vl53l1x

    PRIV_REQUIRES
        waveshare__esp32_p4_wifi6_touch_lcd_7b
        bsp_extra
)

target_compile_options(__idf_ui PRIVATE -Wno-format-truncation)
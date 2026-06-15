idf_component_register(
    SRCS "VL53L1X_api.c" "VL53L1X_calibration.c" "vl53l1_platform.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c tether_config waveshare__esp32_p4_wifi6_touch_lcd_7b
)

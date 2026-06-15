idf_component_register(
    SRCS "device_scan.c"
    INCLUDE_DIRS "."
    REQUIRES tether_config vl53l1x tether-ble storage
)

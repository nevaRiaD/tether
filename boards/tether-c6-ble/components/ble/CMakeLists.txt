idf_component_register(
    SRCS
        "ble_tether.c"
    INCLUDE_DIRS "."
    REQUIRES bt pair log tether_config console filter lib sdio-slave device_scan
)
idf_component_register(
    SRCS
        "device_scan.c"
    INCLUDE_DIRS "."
    REQUIRES bt log tether_config filter accelerometer pair lib
)

idf_component_register(
    SRCS
        "ble_pair.c"
        "ble_pair_console.c"
        "ble_pair_utils.c"
    INCLUDE_DIRS "."
    REQUIRES ble bt filter log tether_config console sdio-slave
)
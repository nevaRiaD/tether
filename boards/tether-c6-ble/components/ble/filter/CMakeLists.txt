idf_component_register(
    SRCS
        "ble_rssi_buff.c"
    INCLUDE_DIRS "."
    REQUIRES bt log tether_config sdio-slave lib pair accelerometer
)

# Force-retain the peer_traverse_all callback: device_scan references it by name
# across a component boundary, so it must not be dropped under MINIMAL_BUILD.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,ble_rssi_move_close")
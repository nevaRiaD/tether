idf_component_register(
    SRCS
        "gatt_svr.c"
        "misc.c"
        "peer.c"
    INCLUDE_DIRS "."
    REQUIRES bt log tether_config console
)
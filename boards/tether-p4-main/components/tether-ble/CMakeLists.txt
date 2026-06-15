idf_component_register(SRCS "tether_ble.c"
                       INCLUDE_DIRS "."
                       REQUIRES sdio-master tether_config esp_serial_slave_link storage)

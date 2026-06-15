idf_component_register(SRCS "tether_sdio_slave.c"
                            "tether_sdio_cmd.c"
                            "tether_sdio_events.c"
                       PRIV_REQUIRES ble esp_driver_sdio esp_ringbuf tether_config pair device_scan
                       INCLUDE_DIRS ".")
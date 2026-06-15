idf_component_register(SRCS "sdio_master.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_sdmmc
                                esp_driver_gpio
                                sdmmc
                                esp_serial_slave_link
                                tether_config)
idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES sdio-slave ble nvs_flash
                       INCLUDE_DIRS ".")

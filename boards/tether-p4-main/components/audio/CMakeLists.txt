idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES log
    PRIV_REQUIRES littlefs bsp_extra
)

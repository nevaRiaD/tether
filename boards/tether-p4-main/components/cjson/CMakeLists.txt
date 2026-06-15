idf_component_register(
    SRCS "cJSON.c"
    INCLUDE_DIRS "."
)

# Suppress warnings inside cJSON itself
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
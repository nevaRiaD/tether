# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)
set(PARTITION_TABLE_CSV_PATH "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tether-project)
littlefs_create_partition_image(audio audio_files FLASH_IN_PROJECT)

# LVGL's LV_ATTRIBUTE_FAST_MEM expands to IRAM_ATTR, whose __COUNTER__-based
# section names differ between a function's declaration and definition. The
# GCC in ESP-IDF v6.0 promotes that mismatch to -Werror=attributes. The
# attribute is still honored (the function lands in IRAM); only demote the
# error so the build succeeds.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-error=attributes)

# esp-audio-player casts to qualified types (e.g. (TaskHandle_t * const)NULL);
# the GCC in ESP-IDF v6.0 promotes the resulting harmless warning to an error.
idf_component_get_property(audio_player_lib chmorgan__esp-audio-player COMPONENT_LIB)
target_compile_options(${audio_player_lib} PRIVATE -Wno-error=ignored-qualifiers)

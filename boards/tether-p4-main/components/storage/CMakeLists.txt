idf_component_register(
    SRCS "src/clock.c"
         "src/schedule.c"
         "src/store.c"
         "src/tag.c"
    # storage_test.c / TEST_schedule.c are stale test harnesses (never called by
    # the app) that use the old tag_algorithm() signature. Excluded from the build.
    INCLUDE_DIRS "include"
    REQUIRES log littlefs cjson tether_config
)
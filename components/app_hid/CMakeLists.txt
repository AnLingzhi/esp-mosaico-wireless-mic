idf_component_register(SRCS "app_hid.c" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private" REQUIRES esp_common
    PRIV_REQUIRES bt freertos log)

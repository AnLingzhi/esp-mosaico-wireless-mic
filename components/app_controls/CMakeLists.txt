idf_component_register(SRCS "app_controls.c" "button_gesture.c" "key_sequence.c"
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "private"
    REQUIRES esp_common
    PRIV_REQUIRES app_hid esp-mosaico-bsp esp_driver_gpio esp_timer freertos log)

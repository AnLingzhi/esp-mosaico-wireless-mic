idf_component_register(
    SRCS "app_touch.c" "battery_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-mosaico-bsp lvgl
    PRIV_REQUIRES app_controls app_hfp app_hid esp_iris freertos esp_driver_i2c)

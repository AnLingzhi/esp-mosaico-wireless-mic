idf_component_register(
    SRCS "app_audio.c" "app_audio_fifo.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES esp-mosaico-bsp
    PRIV_REQUIRES esp_common freertos heap log)

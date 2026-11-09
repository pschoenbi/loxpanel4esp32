idf_component_register(
    SRCS "cam.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_jpeg esp_driver_ppa esp_http_client esp_timer
)

idf_component_register(
    SRCS "lox_client.c" "lox_crypto.c" "lox_struct.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES mbedtls nvs_flash esp_timer esp_hw_support esp_psram esp_http_client
)

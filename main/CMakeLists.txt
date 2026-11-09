idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES boards network loxone ui config settings web
    PRIV_REQUIRES esp_system esp_app_format nvs_flash
)

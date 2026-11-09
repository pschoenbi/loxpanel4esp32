idf_component_register(
    SRCS "cfg.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES loxone nvs_flash esp_rom esp_psram
    EMBED_FILES "../../config/panel.json"
)

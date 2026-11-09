idf_component_register(
    SRCS "ui_main.c" "ui_widgets.c" "ui_ctile.c" "ui_pages.c" "ui_diag.c" "ui_irc.c" "ui_icons.c" "ui_efm.c" "ui_cam.c" "ui_door.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES boards
    PRIV_REQUIRES camera loxone network config settings nvs_flash esp_app_format esp_system esp_hw_support esp_timer
    EMBED_FILES "fonts/Montserrat-Medium.ttf"
)

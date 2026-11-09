set(srcs "board_lvgl.c")

if(CONFIG_LOXPANEL_BOARD_CROWPANEL_P4_9IN)
    list(APPEND srcs "crowpanel_p4_9in/board_crowpanel_p4_9in.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd
    PRIV_REQUIRES esp_driver_ledc esp_driver_i2c esp_driver_gpio esp_hw_support
)

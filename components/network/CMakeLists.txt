set(srcs "net_wifi.c" "net_time.c" "net_dhcp_bcast.c")
set(embed "")

# Einmaliges Update des ESP32-C6 über SDIO (nur wenn ausdrücklich eingeschaltet)
if(CONFIG_LOXPANEL_C6_UPDATE)
    list(APPEND srcs "net_c6_update.c")
    list(APPEND embed "c6_fw/network_adapter_2.12.3.bin")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_hw_support esp_timer esp_app_format bootloader_support lwip
    EMBED_FILES ${embed}
)

# DHCP mit Broadcast-Flag (siehe net_dhcp_bcast.c)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dhcp_append_extra_opts")
# Sicherstellen, dass __wrap_dhcp_append_extra_opts gelinkt wird
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_dhcp_append_extra_opts")

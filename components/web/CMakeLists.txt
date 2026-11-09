idf_component_register(
    SRCS "web.c" "web_dns.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer esp_system esp_app_format mbedtls lwip
                  boards settings network loxone config
    EMBED_TXTFILES "www/index.html"
)

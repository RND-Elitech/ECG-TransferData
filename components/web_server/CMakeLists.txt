idf_component_register(
    SRCS "web_server.c" "dns_server.c"
    INCLUDE_DIRS "."
    EMBED_FILES "root.html" "dashboard.html"
    PRIV_REQUIRES
        esp_http_server
        esp_wifi
        nvs_flash
        esp_netif
        freertos
        json
        lwip
)

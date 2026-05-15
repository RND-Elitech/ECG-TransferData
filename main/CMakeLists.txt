idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager storage_manager uploader console_cmd web_server
    PRIV_REQUIRES nvs_flash esp_netif esp_event mdns esp_wifi
)

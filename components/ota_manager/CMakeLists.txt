idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_client
        esp_https_ota
        json
        esp_wifi
        esp_event
        nvs_flash
        app_update
)

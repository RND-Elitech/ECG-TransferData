idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager storage_manager uploader console_cmd web_server
    PRIV_REQUIRES nvs_flash esp_netif esp_event mdns esp_wifi
)

# Gunakan fitur Linker Wrap untuk menyadap (intercept) fungsi baca/tulis ke SD Card
# Ini memungkinkan kita mendeteksi kapan Mesin EKG benar-benar berhenti mentransfer data
target_link_libraries(${COMPONENT_LIB} "-Wl,--wrap=sdmmc_write_sectors" "-Wl,--wrap=sdmmc_read_sectors")

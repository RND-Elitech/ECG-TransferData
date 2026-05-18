#include "device_info.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "device_info";

esp_err_t device_info_get_sn(char *buffer, size_t max_len) {
    if (!buffer || max_len == 0) return ESP_ERR_INVALID_ARG;

    // Bersihkan buffer di awal
    memset(buffer, 0, max_len);

    nvs_handle_t nvs_handle;
    // Buka namespace "factory" di NVS
    esp_err_t err = nvs_open("factory", NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        size_t required_size = max_len;
        err = nvs_get_str(nvs_handle, "dongle_sn", buffer, &required_size);
        nvs_close(nvs_handle);
        
        if (err == ESP_OK && strlen(buffer) > 0) {
            ESP_LOGI(TAG, "SN didapatkan dari NVS: %s", buffer);
            return ESP_OK;
        }
    }

    // Jika gagal baca dari NVS, fallback membaca MAC Address dari eFuse
    uint8_t mac[6];
    err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err == ESP_OK) {
        snprintf(buffer, max_len, "Dongle-%02X%02X%02X", mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "SN NVS tidak ditemukan. Fallback ke MAC: %s", buffer);
        return ESP_OK;
    }

    // Fallback darurat (seharusnya tidak terjadi)
    snprintf(buffer, max_len, "Dongle-UNKNOWN");
    return ESP_FAIL;
}

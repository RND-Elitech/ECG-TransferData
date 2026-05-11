#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "storage_manager.h"
#include "uploader.h"
#include "console_cmd.h"

static const char *TAG = "app_main";

/* ─── Konfigurasi Perangkat ─── */
#define GATEWAY_SN       "B0001"
#define WIFI_SSID        "Elitech"
#define WIFI_PASSWORD    "wifis1nko"
#define MQTT_BROKER_URI  "mqtts://dev.samelement.com"
#define MQTT_BROKER_PORT 8888
#define MQTT_USERNAME    "iotgateway"
#define MQTT_PASSWORD    "iotgateway10nice"
#define SERVER_UPLOAD_URL "http://192.168.13.156:3000/api/ecg-1200g/upload"
#define STORAGE_BASE_PATH "/data"

void app_main(void)
{
    ESP_LOGI(TAG, "=== ECG Dongle Booting ===");

    /* 1. Inisialisasi NVS dan event loop (prasyarat global) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2. Koneksi Wi-Fi (blokir hingga berhasil, timeout 30 detik) */
    ESP_LOGI(TAG, "Menghubungkan ke Wi-Fi...");
    ESP_ERROR_CHECK(wifi_manager_start(WIFI_SSID, WIFI_PASSWORD, 30000));

    /* 3. Inisialisasi storage & USB MSC */
    ESP_LOGI(TAG, "Inisialisasi storage...");
    ESP_ERROR_CHECK(storage_manager_init());
    ESP_ERROR_CHECK(storage_manager_mount());

    /* 4. Bersihkan file lama, lalu serahkan storage ke USB Host */
    storage_manager_cleanup();
    ESP_ERROR_CHECK(storage_manager_expose_to_usb());
    ESP_LOGI(TAG, "Storage siap di-expose ke USB Host");

    /* 5. Inisialisasi uploader (injeksi konfigurasi server) */
    uploader_init(&(uploader_config_t){
        .server_url = SERVER_UPLOAD_URL,
        .base_path  = STORAGE_BASE_PATH,
    });

    /* 6. Mulai MQTT — callback upload diarahkan ke uploader_trigger */
    ESP_LOGI(TAG, "Memulai MQTT client...");
    ESP_ERROR_CHECK(mqtt_manager_init(&(mqtt_manager_config_t){
        .broker_uri    = MQTT_BROKER_URI,
        .broker_port   = MQTT_BROKER_PORT,
        .username      = MQTT_USERNAME,
        .password      = MQTT_PASSWORD,
        .gateway_sn    = GATEWAY_SN,
        .on_upload_cmd = uploader_trigger,
        .cb_ctx        = NULL,
    }));

    /* 7. Jalankan Console REPL (non-blocking — berjalan sebagai FreeRTOS task) */
    ESP_ERROR_CHECK(console_cmd_init());

    ESP_LOGI(TAG, "=== ECG Dongle Siap ===");
}

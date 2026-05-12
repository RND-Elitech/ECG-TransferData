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
#define GATEWAY_SN       "GWTEST"
#define WIFI_SSID        "Elitech"
#define WIFI_PASSWORD    "wifis1nko"
#define MQTT_BROKER_URI  "mqtts://dev.samelement.com"
#define MQTT_BROKER_PORT 8888
#define MQTT_USERNAME    "iotgateway"
#define MQTT_PASSWORD    "iotgateway10nice"
#define FTP_SERVER_HOST  "192.168.13.145"
#define FTP_SERVER_PORT  21
#define FTP_SERVER_USER  "rival"
#define FTP_SERVER_PASS  "123456"
#define STORAGE_BASE_PATH "/data"
#include "nvs.h"

/* Variabel Global Konfigurasi */
static char s_wifi_ssid[64];
static char s_wifi_pass[64];
static char s_mqtt_uri[128];
static int32_t s_mqtt_port;
static char s_mqtt_user[64];
static char s_mqtt_pass[64];
static char s_gateway_sn[32];
static char s_ftp_host[64];
static int32_t s_ftp_port;
static char s_ftp_user[64];
static char s_ftp_pass[64];

static void get_nvs_str(nvs_handle_t handle, const char *key, char *out_val, size_t max_len, const char *default_val) {
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required_size);
    if (err == ESP_OK && required_size <= max_len) {
        nvs_get_str(handle, key, out_val, &required_size);
    } else {
        strncpy(out_val, default_val, max_len - 1);
        out_val[max_len - 1] = '\0';
    }
}

static int32_t get_nvs_i32(nvs_handle_t handle, const char *key, int32_t default_val) {
    int32_t out_val = default_val;
    nvs_get_i32(handle, key, &out_val);
    return out_val;
}

static void load_config_from_nvs(void) {
    nvs_handle_t nvs_handle;
    if (nvs_open("config", NVS_READONLY, &nvs_handle) == ESP_OK) {
        ESP_LOGI(TAG, "Memuat konfigurasi dari NVS...");
        get_nvs_str(nvs_handle, "wifi_ssid", s_wifi_ssid, sizeof(s_wifi_ssid), WIFI_SSID);
        get_nvs_str(nvs_handle, "wifi_pass", s_wifi_pass, sizeof(s_wifi_pass), WIFI_PASSWORD);
        get_nvs_str(nvs_handle, "mqtt_uri", s_mqtt_uri, sizeof(s_mqtt_uri), MQTT_BROKER_URI);
        s_mqtt_port = get_nvs_i32(nvs_handle, "mqtt_port", MQTT_BROKER_PORT);
        get_nvs_str(nvs_handle, "mqtt_user", s_mqtt_user, sizeof(s_mqtt_user), MQTT_USERNAME);
        get_nvs_str(nvs_handle, "mqtt_pass", s_mqtt_pass, sizeof(s_mqtt_pass), MQTT_PASSWORD);
        get_nvs_str(nvs_handle, "gateway_sn", s_gateway_sn, sizeof(s_gateway_sn), GATEWAY_SN);
        get_nvs_str(nvs_handle, "ftp_host", s_ftp_host, sizeof(s_ftp_host), FTP_SERVER_HOST);
        s_ftp_port = get_nvs_i32(nvs_handle, "ftp_port", FTP_SERVER_PORT);
        get_nvs_str(nvs_handle, "ftp_user", s_ftp_user, sizeof(s_ftp_user), FTP_SERVER_USER);
        get_nvs_str(nvs_handle, "ftp_pass", s_ftp_pass, sizeof(s_ftp_pass), FTP_SERVER_PASS);
        nvs_close(nvs_handle);
    } else {
        ESP_LOGI(TAG, "NVS config tidak ditemukan, menggunakan nilai default.");
        strncpy(s_wifi_ssid, WIFI_SSID, sizeof(s_wifi_ssid)-1);
        strncpy(s_wifi_pass, WIFI_PASSWORD, sizeof(s_wifi_pass)-1);
        strncpy(s_mqtt_uri, MQTT_BROKER_URI, sizeof(s_mqtt_uri)-1);
        s_mqtt_port = MQTT_BROKER_PORT;
        strncpy(s_mqtt_user, MQTT_USERNAME, sizeof(s_mqtt_user)-1);
        strncpy(s_mqtt_pass, MQTT_PASSWORD, sizeof(s_mqtt_pass)-1);
        strncpy(s_gateway_sn, GATEWAY_SN, sizeof(s_gateway_sn)-1);
        strncpy(s_ftp_host, FTP_SERVER_HOST, sizeof(s_ftp_host)-1);
        s_ftp_port = FTP_SERVER_PORT;
        strncpy(s_ftp_user, FTP_SERVER_USER, sizeof(s_ftp_user)-1);
        strncpy(s_ftp_pass, FTP_SERVER_PASS, sizeof(s_ftp_pass)-1);
    }
}

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

    /* 1.1 Muat Konfigurasi dari NVS */
    load_config_from_nvs();

    /* 2. Koneksi Wi-Fi (blokir hingga berhasil, timeout 30 detik) */
    ESP_LOGI(TAG, "Menghubungkan ke Wi-Fi...");
    ESP_ERROR_CHECK(wifi_manager_start(s_wifi_ssid, s_wifi_pass, 30000));

    /* 3. Inisialisasi storage & USB MSC */
    ESP_LOGI(TAG, "Inisialisasi storage...");
    ESP_ERROR_CHECK(storage_manager_init());
    ESP_ERROR_CHECK(storage_manager_mount());

    /* 4. Serahkan storage ke USB Host */
    ESP_ERROR_CHECK(storage_manager_expose_to_usb());
    ESP_LOGI(TAG, "Storage siap di-expose ke USB Host");

    /* 5. Inisialisasi uploader (injeksi konfigurasi FTP) */
    uploader_init(&(uploader_config_t){
        .ftp_host  = s_ftp_host,
        .ftp_port  = s_ftp_port,
        .ftp_user  = s_ftp_user,
        .ftp_pass  = s_ftp_pass,
        .base_path = STORAGE_BASE_PATH,
    });

    /* 6. Mulai MQTT — callback upload diarahkan ke uploader_trigger */
    ESP_LOGI(TAG, "Memulai MQTT client...");
    ESP_ERROR_CHECK(mqtt_manager_init(&(mqtt_manager_config_t){
        .broker_uri    = s_mqtt_uri,
        .broker_port   = s_mqtt_port,
        .username      = s_mqtt_user,
        .password      = s_mqtt_pass,
        .gateway_sn    = s_gateway_sn,
        .on_upload_cmd = uploader_trigger,

        .cb_ctx        = NULL,
    }));

    /* 7. Jalankan Console REPL (non-blocking — berjalan sebagai FreeRTOS task) */
    ESP_ERROR_CHECK(console_cmd_init());

    ESP_LOGI(TAG, "=== ECG Dongle Siap ===");
}

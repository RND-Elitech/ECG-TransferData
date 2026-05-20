#include "wifi_manager.h"
#include "device_info.h"

#include <string.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_manager";

/* Bit flag untuk event group */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static bool s_wifi_initialized  = false;
static volatile bool s_is_connected = false;
static int s_retry_num = 0;
#define MAX_WIFI_RETRY 3
static volatile bool s_auto_reconnect = false;

bool wifi_manager_is_connected(void) {
    return s_is_connected;
}

static void _init_wifi_if_needed(void) {
    if (!s_wifi_initialized) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        s_ap_netif = esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_initialized = true;
    }
}

/* ─────────────────────────────────────────────────────────────
 * Event Handler (STA mode)
 * ───────────────────────────────────────────────────────────── */
static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi terputus! Alasan: %d", disconn->reason);

        if (s_wifi_event_group) {
            // Kasus A: Sedang dalam proses inisialisasi koneksi awal (wifi_manager_start)
            if (s_retry_num < MAX_WIFI_RETRY) {
                s_retry_num++;
                ESP_LOGI(TAG, "Mencoba menyambung kembali ke AP (%d/%d)...", s_retry_num, MAX_WIFI_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Gagal terhubung ke WiFi setelah %d percobaan.", MAX_WIFI_RETRY);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else {
            // Kasus B: Beroperasi normal (sebelumnya sudah sukses terhubung)
            if (s_auto_reconnect) {
                ESP_LOGW(TAG, "Koneksi terputus di tengah jalan. Mencoba menyambung kembali (Auto-Reconnect)...");
                esp_wifi_connect();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_is_connected = true;
        s_retry_num = 0;
        s_auto_reconnect = true; // Aktifkan auto-reconnect setelah sukses mendapatkan IP
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/* ─────────────────────────────────────────────────────────────
 * STA Mode: wifi_manager_start()
 * ───────────────────────────────────────────────────────────── */
esp_err_t wifi_manager_start(const char *ssid, const char *password,
                             uint32_t timeout_ms) {
    s_retry_num = 0;
    s_auto_reconnect = false;
    s_wifi_event_group = xEventGroupCreate();

    _init_wifi_if_needed();

    /* Register event handlers */
    static bool s_event_handlers_registered = false;
    if (!s_event_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_sta_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_sta_event_handler, NULL, NULL));
        s_event_handlers_registered = true;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    char ap_ssid[32];
    device_info_get_sn(ap_ssid, sizeof(ap_ssid));
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len       = 0,
            .channel        = 1,
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = 4,
            .beacon_interval= 100,
        },
    };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Hanya perbarui konfigurasi AP jika belum dikonfigurasi dengan benar
    // untuk mencegah klien (HP user) terputus dari Captive Portal.
    wifi_config_t current_ap_config;
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &current_ap_config);
    if (err != ESP_OK || strncmp((char *)current_ap_config.ap.ssid, ap_ssid, sizeof(ap_ssid)) != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    // Coba hubungkan secara eksplisit jika Wi-Fi driver sudah berjalan sebelumnya
    esp_err_t conn_err = esp_wifi_connect();
    if (conn_err == ESP_OK) {
        ESP_LOGI(TAG, "Koneksi ke WiFi SSID: '%s' diinisiasi langsung.", ssid);
    } else {
        ESP_LOGI(TAG, "Menghubungkan ke WiFi SSID: '%s' ... (menunggu event)", ssid);
    }

    /* Tunggu hingga connected atau timeout */
    TickType_t ticks_to_wait = (timeout_ms > 0)
                               ? pdMS_TO_TICKS(timeout_ms)
                               : portMAX_DELAY;

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           ticks_to_wait);

    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Berhasil terhubung ke WiFi: '%s'", ssid);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Gagal terhubung ke WiFi: '%s'", ssid);
        return ESP_ERR_WIFI_CONN;
    } else {
        ESP_LOGE(TAG, "Timeout koneksi WiFi setelah %lu ms", (unsigned long)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
}

/* ─────────────────────────────────────────────────────────────
 * AP Mode: wifi_manager_start_ap()
 * ───────────────────────────────────────────────────────────── */
esp_err_t wifi_manager_start_ap(void) {
    s_auto_reconnect = false;
    /* Dapatkan 2 byte terakhir MAC untuk SSID unik */
    /* Dapatkan SN perangkat untuk nama AP */
    char ap_ssid[32];
    device_info_get_sn(ap_ssid, sizeof(ap_ssid));

    _init_wifi_if_needed();

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len       = 0,       /* auto-detect dari ssid string */
            .channel        = 1,
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = 4,
            .beacon_interval= 100,
        },
    };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP Mode aktif — SSID: '%s', IP: 192.168.4.1 (open)", ap_ssid);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────
 * Stop WiFi
 * ───────────────────────────────────────────────────────────── */
void wifi_manager_stop(void) {
    s_is_connected = false;
    s_auto_reconnect = false;
    esp_wifi_stop();
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        if (s_sta_netif) {
            esp_netif_destroy(s_sta_netif);
            s_sta_netif = NULL;
        }
        if (s_ap_netif) {
            esp_netif_destroy(s_ap_netif);
            s_ap_netif = NULL;
        }
        s_wifi_initialized = false;
    }
    ESP_LOGI(TAG, "WiFi dihentikan");
}

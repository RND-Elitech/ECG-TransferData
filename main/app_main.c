#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RESET_BTN_PIN GPIO_NUM_1

static void reset_btn_task(void *arg) {
  gpio_config_t io_conf = {.pin_bit_mask = (1ULL << RESET_BTN_PIN),
                           .mode = GPIO_MODE_INPUT,
                           .pull_up_en = 1,
                           .pull_down_en = 0,
                           .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&io_conf);

  int press_count = 0;
  while (1) {
    if (gpio_get_level(RESET_BTN_PIN) == 0) { // Ditekan (active low)
      press_count++;
      if (press_count >= 30) { // Tahan 3 detik
        ESP_LOGW("reset_btn",
                 "Tombol Reset ditekan 3 detik. Menghapus konfigurasi...");
        nvs_handle_t nvs_h;
        if (nvs_open("config", NVS_READWRITE, &nvs_h) == ESP_OK) {
          nvs_erase_all(nvs_h);
          nvs_commit(nvs_h);
          nvs_close(nvs_h);
        }
        ESP_LOGW("reset_btn", "Konfigurasi dihapus. Restarting ESP32...");
        esp_restart();
      }
    } else {
      press_count = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

#include "console_cmd.h"
#include "dns_server.h"
#include "mqtt_manager.h"
#include "storage_manager.h"
#include "uploader.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "app_main";

/* ─── Konfigurasi Default (hanya untuk MQTT & SN) ─── */
#define GATEWAY_SN "GWTEST"
#define MQTT_BROKER_URI "mqtts://dev.samelement.com"
#define MQTT_BROKER_PORT 8888
#define MQTT_USERNAME "iotgateway"
#define MQTT_PASSWORD "iotgateway10nice"
#define STORAGE_BASE_PATH "/data"

/* ─── Variabel Global Konfigurasi ─── */
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

/* ─── NVS Helpers ─── */
static void get_nvs_str(nvs_handle_t handle, const char *key, char *out_val,
                        size_t max_len, const char *default_val) {
  size_t required_size = 0;
  esp_err_t err = nvs_get_str(handle, key, NULL, &required_size);
  if (err == ESP_OK && required_size <= max_len) {
    nvs_get_str(handle, key, out_val, &required_size);
  } else {
    strncpy(out_val, default_val, max_len - 1);
    out_val[max_len - 1] = '\0';
  }
}

static int32_t get_nvs_i32(nvs_handle_t handle, const char *key,
                           int32_t default_val) {
  int32_t out_val = default_val;
  nvs_get_i32(handle, key, &out_val);
  return out_val;
}

static bool load_config_from_nvs(void) {
  bool has_config = false;
  nvs_handle_t nvs_handle;
  if (nvs_open("config", NVS_READONLY, &nvs_handle) == ESP_OK) {
    ESP_LOGI(TAG, "Memuat konfigurasi dari NVS...");

    size_t required_size = 0;
    if (nvs_get_str(nvs_handle, "wifi_ssid", NULL, &required_size) == ESP_OK) {
      has_config = true;
    }

    get_nvs_str(nvs_handle, "wifi_ssid", s_wifi_ssid, sizeof(s_wifi_ssid), "");
    get_nvs_str(nvs_handle, "wifi_pass", s_wifi_pass, sizeof(s_wifi_pass), "");
    get_nvs_str(nvs_handle, "mqtt_uri", s_mqtt_uri, sizeof(s_mqtt_uri),
                MQTT_BROKER_URI);
    s_mqtt_port = get_nvs_i32(nvs_handle, "mqtt_port", MQTT_BROKER_PORT);
    get_nvs_str(nvs_handle, "mqtt_user", s_mqtt_user, sizeof(s_mqtt_user),
                MQTT_USERNAME);
    get_nvs_str(nvs_handle, "mqtt_pass", s_mqtt_pass, sizeof(s_mqtt_pass),
                MQTT_PASSWORD);
    get_nvs_str(nvs_handle, "gateway_sn", s_gateway_sn, sizeof(s_gateway_sn),
                GATEWAY_SN);
    get_nvs_str(nvs_handle, "ftp_host", s_ftp_host, sizeof(s_ftp_host), "");
    s_ftp_port = get_nvs_i32(nvs_handle, "ftp_port", 21);
    get_nvs_str(nvs_handle, "ftp_user", s_ftp_user, sizeof(s_ftp_user), "");
    get_nvs_str(nvs_handle, "ftp_pass", s_ftp_pass, sizeof(s_ftp_pass), "");
    nvs_close(nvs_handle);
  } else {
    ESP_LOGW(TAG, "NVS 'config' tidak ditemukan — menggunakan nilai default.");
    s_wifi_ssid[0] = '\0';
    s_wifi_pass[0] = '\0';
    strncpy(s_mqtt_uri, MQTT_BROKER_URI, sizeof(s_mqtt_uri) - 1);
    s_mqtt_port = MQTT_BROKER_PORT;
    strncpy(s_mqtt_user, MQTT_USERNAME, sizeof(s_mqtt_user) - 1);
    strncpy(s_mqtt_pass, MQTT_PASSWORD, sizeof(s_mqtt_pass) - 1);
    strncpy(s_gateway_sn, GATEWAY_SN, sizeof(s_gateway_sn) - 1);
    s_ftp_host[0] = '\0';
    s_ftp_port = 21;
    s_ftp_user[0] = '\0';
    s_ftp_pass[0] = '\0';
  }
  return has_config;
}

/* ─────────────────────────────────────────────────────────────
 * Fallback: Mode AP + Captive Portal
 *
 * Dipanggil jika koneksi WiFi gagal. ESP32 berfungsi sebagai
 * Access Point "ECG-Gateway-XXXX" dan menampilkan portal
 * konfigurasi di browser pengguna secara otomatis.
 * ───────────────────────────────────────────────────────────── */
static void enter_ap_config_mode(void) {
  ESP_LOGW(TAG, "=== Memasuki Mode Konfigurasi (AP + Captive Portal) ===");
  ESP_LOGW(TAG, "  Hubungkan HP/Laptop ke WiFi: ECG-Gateway-XXXX");
  ESP_LOGW(TAG, "  Buka browser: http://192.168.4.1");

  /* Aktifkan Access Point */
  esp_err_t err = wifi_manager_start_ap();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Gagal memulai AP mode: %s", esp_err_to_name(err));
    return;
  }

  /* Jalankan DNS Server — membuat HP otomatis popup portal */
  err = dns_server_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Gagal memulai DNS server: %s", esp_err_to_name(err));
    /* Lanjut saja — portal masih bisa diakses via 192.168.4.1 */
  }

  /* Jalankan HTTP Web Server */
  err = web_server_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Gagal memulai Web server: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(TAG, "Portal konfigurasi aktif. Menunggu input pengguna...");
  /* Task selesai — web server dan dns server berjalan di background
   * task/interrupt */
}

/* ─────────────────────────────────────────────────────────────
 * Normal Boot: Koneksi WiFi sukses
 * ───────────────────────────────────────────────────────────── */
static void run_normal_operation(void) {
  /* 3. Inisialisasi storage & USB MSC */
  ESP_LOGI(TAG, "Inisialisasi storage...");
  ESP_ERROR_CHECK(storage_manager_init());
  ESP_ERROR_CHECK(storage_manager_mount());

  /* 4. Serahkan storage ke USB Host */
  ESP_ERROR_CHECK(storage_manager_expose_to_usb());
  ESP_LOGI(TAG, "Storage siap di-expose ke USB Host");

  /* 5. Inisialisasi uploader (injeksi konfigurasi FTP) */
  uploader_init(&(uploader_config_t){
      .ftp_host = s_ftp_host,
      .ftp_port = s_ftp_port,
      .ftp_user = s_ftp_user,
      .ftp_pass = s_ftp_pass,
      .base_path = STORAGE_BASE_PATH,
  });

  /* 6. Mulai MQTT */
  ESP_LOGI(TAG, "Memulai MQTT client...");
  ESP_ERROR_CHECK(mqtt_manager_init(&(mqtt_manager_config_t){
      .broker_uri = s_mqtt_uri,
      .broker_port = s_mqtt_port,
      .username = s_mqtt_user,
      .password = s_mqtt_pass,
      .gateway_sn = s_gateway_sn,
      .on_upload_cmd = uploader_trigger,
      .cb_ctx = NULL,
  }));

  /* 7. Jalankan Console REPL */
  ESP_ERROR_CHECK(console_cmd_init());

  ESP_LOGI(TAG, "=== ECG Dongle Siap ===");
}

/* ─────────────────────────────────────────────────────────────
 * app_main — Entry Point
 * ───────────────────────────────────────────────────────────── */
void app_main(void) {
  ESP_LOGI(TAG, "=== ECG Dongle Booting ===");

  /* Mulai task pemantau tombol reset konfigurasi */
  xTaskCreate(reset_btn_task, "reset_btn", 2048, NULL, 5, NULL);

  /* 1. Inisialisasi NVS dan event loop (prasyarat global) */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  /* 1.1 Muat Konfigurasi dari NVS */
  bool has_config = load_config_from_nvs();

  /* 1.2 Cek apakah konfigurasi WiFi kosong (Device baru) */
  if (!has_config || strlen(s_wifi_ssid) == 0) {
    ESP_LOGW(TAG, "Konfigurasi kosong! Masuk Mode Konfigurasi (AP)...");
    enter_ap_config_mode();
    return;
  }

  /* 2. Coba koneksi WiFi (timeout 30 detik) */
  ESP_LOGI(TAG, "Menghubungkan ke WiFi SSID: '%s' ...", s_wifi_ssid);
  ret = wifi_manager_start(s_wifi_ssid, s_wifi_pass, 30000);

  if (ret != ESP_OK) {
    /*
     * Koneksi WiFi GAGAL — masuk Mode AP agar pengguna
     * bisa memasukkan konfigurasi baru melalui browser.
     */
    ESP_LOGW(TAG, "WiFi gagal terkoneksi (%s). Beralih ke Mode AP...",
             esp_err_to_name(ret));
    enter_ap_config_mode();
    return;
  }

  /* WiFi OK — boot normal */
  ESP_LOGI(TAG, "WiFi terhubung! Melanjutkan proses normal...");
  run_normal_operation();
}

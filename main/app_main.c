#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <string.h>
#include <unistd.h>

#include "console_cmd.h"
#include "dns_server.h"
#include "led_strip.h"
#include "mdns.h"
#include "ota_manager.h"
#include "sdmmc_cmd.h"
#include "storage_manager.h"
#include "uploader.h"
#include "web_server.h"
#include "wifi_manager.h"

#define RESET_BTN_PIN GPIO_NUM_1
#define BOOT_BTN_PIN GPIO_NUM_0
#define LED_PIN GPIO_NUM_48 ///< Indikator status upload

/* ─────────────────────────────────────────────────────────────
 * LED Indicator (Smart RGB LED WS2812 — GPIO 48)
 * State:
 *   OFF              = Mati
 *   STANDBY          = Putih Solid — Terhubung WiFi, siap menerima data EKG
 *   WIFI_CONNECTING  = Putih Berkedip Cepat — Sedang mencari/konek ke WiFi
 *   AP_MODE          = Putih Berkedip Lambat — Mode Konfigurasi (AP aktif)
 *   ERROR            = Merah Solid — Koneksi WiFi / FTP gagal
 *   BLINK_SLOW       = Biru Berkedip Lambat — Safeguard 1 detik aktif
 *   BLINK_FAST       = Biru Berkedip Cepat — Sedang upload ke FTP
 *   ON_SOLID         = Hijau Solid (2 detik) — Upload sukses
 * ───────────────────────────────────────────────────────────── */
typedef enum {
  LED_OFF = 0,
  LED_STANDBY,         // Putih Solid — Standby
  LED_WIFI_CONNECTING, // Putih Berkedip Cepat — Mencari WiFi
  LED_AP_MODE,         // Putih Berkedip Lambat — Mode Konfigurasi/AP
  LED_ERROR_WIFI,      // Merah Berkedip 1x — WiFi Gagal
  LED_ERROR_FTP,       // Merah Berkedip 2x — FTP Koneksi/Login Gagal
  LED_ERROR_UPLOAD,    // Merah Berkedip 3x — Transfer File Gagal
  LED_BLINK_SLOW,      // Biru Berkedip Lambat — Safeguard
  LED_BLINK_FAST,      // Biru Berkedip Cepat — Uploading
  LED_ON_SOLID,        // Hijau Menyala Terus — Sukses
} led_state_t;

static volatile led_state_t s_led_state = LED_OFF;
static volatile bool s_normal_op_running = false;

static void led_task(void *arg) {
  /* Konfigurasi untuk Smart RGB LED (WS2812) via RMT */
  led_strip_handle_t led_strip;
  led_strip_config_t strip_config = {
      .strip_gpio_num = LED_PIN,
      .max_leds = 1, // Hanya ada 1 LED RGB di board
  };
  led_strip_rmt_config_t rmt_config = {
      .resolution_hz = 10 * 1000 * 1000, // Resolusi 10MHz
      .flags.with_dma = false,
  };
  ESP_ERROR_CHECK(
      led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

  // Matikan LED di awal
  led_strip_clear(led_strip);

  while (1) {
    led_state_t current_state = s_led_state;

    // Jika status harusnya standby tapi WiFi terputus, ganti ke indikator error WiFi
    if (current_state == LED_STANDBY && !wifi_manager_is_connected()) {
        current_state = LED_ERROR_WIFI;
    }

    switch (current_state) {
    case LED_OFF:
      led_strip_clear(led_strip);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;
    case LED_STANDBY:
      // Putih Terang (Standby)
      led_strip_set_pixel(led_strip, 0, 255, 255, 255);
      led_strip_refresh(led_strip);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;
    case LED_WIFI_CONNECTING:
      // Putih Berkedip Cepat (Mencari WiFi)
      led_strip_set_pixel(led_strip, 0, 255, 255, 255);
      led_strip_refresh(led_strip);
      vTaskDelay(pdMS_TO_TICKS(100));
      led_strip_clear(led_strip);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;
    case LED_AP_MODE:
      // Putih Berkedip Lambat (Mode AP/Config)
      led_strip_set_pixel(led_strip, 0, 255, 255, 255);
      led_strip_refresh(led_strip);
      vTaskDelay(pdMS_TO_TICKS(500));
      led_strip_clear(led_strip);
      vTaskDelay(pdMS_TO_TICKS(500));
      break;
    case LED_ERROR_WIFI:
      // Merah 1x Kedipan (WiFi Error)
      led_strip_set_pixel(led_strip, 0, 255, 0, 0);
      led_strip_refresh(led_strip);
      vTaskDelay(pdMS_TO_TICKS(200));
      led_strip_clear(led_strip);
      vTaskDelay(pdMS_TO_TICKS(800));
      break;
    case LED_ERROR_FTP:
      // Merah 2x Kedipan (FTP Conn/Login Error)
      for (int i = 0; i < 2; i++) {
        led_strip_set_pixel(led_strip, 0, 255, 0, 0);
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(200));
        led_strip_clear(led_strip);
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      vTaskDelay(pdMS_TO_TICKS(600));
      break;
    case LED_ERROR_UPLOAD:
      // Merah 3x Kedipan (Upload File Error)
      for (int i = 0; i < 3; i++) {
        led_strip_set_pixel(led_strip, 0, 255, 0, 0);
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(200));
        led_strip_clear(led_strip);
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      vTaskDelay(pdMS_TO_TICKS(400));
      break;
    case LED_BLINK_SLOW:
      // Biru Berkedip Lambat (Safeguard)
      led_strip_set_pixel(led_strip, 0, 0, 0, 255);
      led_strip_refresh(led_strip);
      vTaskDelay(pdMS_TO_TICKS(500));
      led_strip_clear(led_strip);
      vTaskDelay(pdMS_TO_TICKS(500));
      break;
    case LED_BLINK_FAST:
      // Biru Berkedip Cepat Maksimal (Uploading)
      led_strip_set_pixel(led_strip, 0, 0, 0, 255);
      led_strip_refresh(led_strip);
      vTaskDelay(pdMS_TO_TICKS(100));
      led_strip_clear(led_strip);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;
    case LED_ON_SOLID:
      // Hijau Menyala Terus Maksimal (Sukses)
      led_strip_set_pixel(led_strip, 0, 0, 255, 0);
      led_strip_refresh(led_strip);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;
    }
  }
}

/* ─────────────────────────────────────────────────────────────
 * SD Card Linker Wrappers (Interceptor)
 * ───────────────────────────────────────────────────────────── */
volatile uint32_t g_last_sd_activity_ms = 0;
volatile bool g_has_new_writes = false;

esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *card, const void *src,
                                     size_t start_block, size_t block_count);
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src,
                                     size_t start_block, size_t block_count) {
  // Hanya catat jika USB Host (PC/Mesin EKG) yang melakukan akses
  if (storage_manager_in_use_by_usb()) {
    g_last_sd_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    g_has_new_writes = true; // Tandai bahwa ada data baru yang ditulis
  }
  return __real_sdmmc_write_sectors(card, src, start_block, block_count);
}

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *card, void *dst,
                                    size_t start_block, size_t block_count);
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst,
                                    size_t start_block, size_t block_count) {
  if (storage_manager_in_use_by_usb()) {
    // Tetap catat aktivitas baca agar waktu idle tidak meleset saat EKG sedang
    // memverifikasi file
    g_last_sd_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  }
  return __real_sdmmc_read_sectors(card, dst, start_block, block_count);
}

/* ─────────────────────────────────────────────────────────────
 * Idle Detector Task (Physical Activity Based)
 *
 * Algoritma Baru (Hardware Intercept):
 *  1. Pantau g_last_sd_activity_ms. Jika berubah, berarti mesin EKG sedang
 * membaca/menulis.
 *  2. Jika mesin EKG sibuk, LED mati (OFF).
 *  3. Jika waktu sejak aktivitas terakhir mencapai 1 detik (Safeguard),
 *     maka bisa dipastikan mesin EKG benar-benar telah selesai.
 *  4. Ambil alih USB, eksekusi Upload.
 * ───────────────────────────────────────────────────────────── */
#define IDLE_SAFEGUARD_MS 1000 // Jeda aman 1 detik setelah write terakhir
#define IDLE_POLL_MS 500       // Cek setiap 500ms

static void idle_detector_task(void *arg) {
  static const char *IDLE_TAG = "idle_det";
  bool is_monitoring_idle = false;

  while (1) {
    // Cek apakah USB Host terhubung (secara kelistrikan/mounting)
    if (!storage_manager_in_use_by_usb()) {
      is_monitoring_idle = false;
      vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
      continue;
    }

    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t time_since_last_activity = current_time - g_last_sd_activity_ms;

    // Hanya mulai pantau masa tenang JIKA ADA PENULISAN (Write)
    if (g_has_new_writes) {
      if (time_since_last_activity < IDLE_SAFEGUARD_MS) {
        if (!is_monitoring_idle) {
          ESP_LOGI(IDLE_TAG,
                   "Transfer data sedang berlangsung... Upload otomatis akan "
                   "terpicu jika mesin hening total selama %d detik.",
                   IDLE_SAFEGUARD_MS / 1000);
          is_monitoring_idle = true;
        }
        s_led_state = LED_BLINK_SLOW;
      } else if (is_monitoring_idle) {
        // Masa tenang sudah terlewati
        ESP_LOGW(IDLE_TAG,
                 "Masa tenang %d detik tercapai. Eksekusi Auto-Upload!",
                 IDLE_SAFEGUARD_MS / 1000);

        is_monitoring_idle = false;
        g_has_new_writes = false; // Reset flag write
        g_last_sd_activity_ms = 0;

        s_led_state = LED_BLINK_FAST;
        uploader_trigger(NULL);
        vTaskDelay(pdMS_TO_TICKS(500)); // Beri waktu uploader untuk start

        // Tunggu sampai uploader selesai bekerja
        for (int w = 0; w < 300; w++) {
          vTaskDelay(pdMS_TO_TICKS(1000));
          if (!uploader_is_busy())
            break;
        }

        // Tampilkan indikator hasil upload
        esp_err_t upload_status = uploader_get_last_status();
        if (upload_status == ESP_OK) {
            s_led_state = LED_ON_SOLID;
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else if (upload_status == UPLOADER_ERR_NO_FILES) {
            // Abaikan tanpa indikator error, langsung kembali ke standby
            ESP_LOGI(IDLE_TAG, "Tidak ada file ECG valid untuk diupload. Mengabaikan.");
        } else if (upload_status == UPLOADER_ERR_FTP_CONN || upload_status == UPLOADER_ERR_FTP_LOGIN) {
            s_led_state = LED_ERROR_FTP;
            vTaskDelay(pdMS_TO_TICKS(4000));
        } else {
            // Error saat transfer file individu (atau error lain)
            s_led_state = LED_ERROR_UPLOAD;
            vTaskDelay(pdMS_TO_TICKS(4000));
        }
        
        s_led_state = LED_STANDBY;
        ESP_LOGI(IDLE_TAG, "Kembali ke mode siaga.");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
  }
}

static void ap_timeout_task(void *arg);

static void boot_btn_task(void *arg) {
  gpio_config_t io_conf = {.pin_bit_mask = (1ULL << BOOT_BTN_PIN),
                           .mode = GPIO_MODE_INPUT,
                           .pull_up_en = 1,
                           .pull_down_en = 0,
                           .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&io_conf);

  int press_count = 0;
  while (1) {
    if (gpio_get_level(BOOT_BTN_PIN) == 0) { // Ditekan (active low)
      press_count++;
      if (press_count >= 30) { // Tahan 3 detik
        ESP_LOGW("boot_btn",
                 "Tombol BOOT ditekan 3 detik. Mengaktifkan mode APSTA...");

        extern int32_t g_ap_timeout_sec;
        if (g_ap_timeout_sec <= 0) {
          // Aktifkan mode APSTA
          esp_wifi_set_mode(WIFI_MODE_APSTA);
          s_led_state = LED_AP_MODE;

          // Siapkan dan jalankan dashboard portal
          web_server_start();
          web_server_set_dashboard_mode(true);
          dns_server_start();

          // Set durasi timeout (2 menit)
          g_ap_timeout_sec = 120;

          // Create task timeout
          xTaskCreate(ap_timeout_task, "ap_timeout", 8192, NULL, 5, NULL);
        } else {
          // Jika APSTA sedang aktif, perpanjang waktu saja
          g_ap_timeout_sec = 120;
          ESP_LOGI("boot_btn",
                   "APSTA sudah aktif. Memperpanjang waktu timeout.");
        }

        // Tunggu sampai tombol dilepas untuk menghindari trigger berulang
        while (gpio_get_level(BOOT_BTN_PIN) == 0) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        press_count = 0;
      }
    } else {
      press_count = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

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

int32_t g_ap_timeout_sec = 0; // 0 = AP mode tidak aktif

static void ap_timeout_task(void *arg) {
  bool ota_pause_logged = false;

  while (g_ap_timeout_sec > 0) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* ── Jeda countdown jika OTA sedang berjalan ─────────────────────────
     * Ini memastikan AP tidak dimatikan di tengah proses download firmware,
     * yang akan memutus koneksi WiFi dan menggagalkan OTA.
     * ─────────────────────────────────────────────────────────────────── */
    ota_state_t ota_state = ota_manager_get_state();
    if (ota_state == OTA_STATE_CHECKING || ota_state == OTA_STATE_DOWNLOADING) {
      if (!ota_pause_logged) {
        ESP_LOGI("ap_timeout", "OTA sedang berjalan — countdown AP ditahan hingga OTA selesai.");
        ota_pause_logged = true;
      }
      /* Tidak decrement: countdown AP berhenti selama OTA aktif */
      continue;
    }

    /* OTA sudah selesai/tidak berjalan — reset flag log dan lanjutkan countdown */
    if (ota_pause_logged) {
      ESP_LOGI("ap_timeout", "OTA selesai — countdown AP dilanjutkan (%"PRId32" detik sisa).",
               g_ap_timeout_sec);
      ota_pause_logged = false;
    }

    g_ap_timeout_sec--;
  }

  if (s_normal_op_running) {
    ESP_LOGI("ap_timeout", "Waktu AP habis. Mematikan AP mode...");

    /* Hentikan web server dengan aman sebelum mematikan AP.
       Ini mencegah lwIP tiba-tiba memutuskan socket yang sedang diproses httpd,
       yang memicu bug double-free/heap corruption di ESP-IDF. */
    web_server_stop();

    web_server_set_dashboard_mode(false);
    dns_server_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_led_state = LED_STANDBY;
  } else {
    ESP_LOGW("ap_timeout", "Koneksi WiFi gagal dan AP timeout habis. Restarting device...");
    esp_restart();
  }

  vTaskDelete(NULL);
}


static const char *TAG = "app_main";

#define STORAGE_BASE_PATH "/data"

/* ─── Variabel Global Konfigurasi ─── */
static char s_wifi_ssid[64];
static char s_wifi_pass[64];
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
    get_nvs_str(nvs_handle, "ftp_host", s_ftp_host, sizeof(s_ftp_host), "");
    s_ftp_port = get_nvs_i32(nvs_handle, "ftp_port", 0);
    get_nvs_str(nvs_handle, "ftp_user", s_ftp_user, sizeof(s_ftp_user), "");
    get_nvs_str(nvs_handle, "ftp_pass", s_ftp_pass, sizeof(s_ftp_pass), "");
    nvs_close(nvs_handle);
  } else {
    ESP_LOGW(TAG, "NVS 'config' tidak ditemukan — menggunakan nilai default.");
    s_wifi_ssid[0] = '\0';
    s_wifi_pass[0] = '\0';
    s_ftp_host[0] = '\0';
    s_ftp_port = 0;
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
static void enter_ap_config_mode(bool enable_timeout) {
  s_led_state = LED_AP_MODE;
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

  if (enable_timeout) {
    g_ap_timeout_sec = 120;
    xTaskCreate(ap_timeout_task, "ap_timeout", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "AP Timeout diaktifkan: 120 detik.");
  } else {
    g_ap_timeout_sec = 0;
    ESP_LOGI(TAG, "AP Timeout dinonaktifkan (indefinite).");
  }

  ESP_LOGI(TAG, "Portal konfigurasi aktif. Menunggu input pengguna...");
  /* Task selesai — web server dan dns server berjalan di background
   * task/interrupt */
}

/* ─────────────────────────────────────────────────────────────
 * Normal Boot: Koneksi WiFi sukses
 * ───────────────────────────────────────────────────────────── */
static void run_normal_operation(void) {
  s_normal_op_running = true;
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

  /* 7. Jalankan Console REPL */
  ESP_ERROR_CHECK(console_cmd_init());

  /* 8. Mulai mDNS */
  ESP_LOGI(TAG, "Memulai mDNS service...");
  ESP_ERROR_CHECK(mdns_init());
  mdns_hostname_set("medlink-dongle");
  mdns_instance_name_set("MedLink Dongle ECG");
  /* 9. Web Server dimatikan saat mode normal (hanya aktif jika tombol BOOT ditekan) */
  // web_server_start(); dihapus sesuai permintaan

  /* 12. Mulai Idle Detector (auto-upload saat USB idle 30 detik) */
  xTaskCreate(idle_detector_task, "idle_det", 4096, NULL, 4, NULL);

  s_led_state = LED_STANDBY;
  ESP_LOGI(TAG, "=== ECG Dongle Siap ===");
}

static void clean_storage_on_boot(void) {
  ESP_LOGI("boot_clean", "Memeriksa sisa file lama di storage...");

  // Inisialisasi dan mount storage agar bisa diakses aplikasi
  if (storage_manager_init() != ESP_OK) {
    ESP_LOGE("boot_clean", "Gagal inisialisasi storage untuk pembersihan.");
    return;
  }
  if (storage_manager_mount() != ESP_OK) {
    ESP_LOGE("boot_clean", "Gagal mount storage untuk pembersihan.");
    return;
  }

  DIR *dir = opendir("/data");
  if (!dir) {
    ESP_LOGE("boot_clean", "Gagal membuka direktori /data");
    return;
  }

  struct dirent *entry;
  bool deleted_any = false;
  while ((entry = readdir(dir)) != NULL) {
    // Skip "." dan ".."
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char path[512];
    snprintf(path, sizeof(path), "/data/%s", entry->d_name);

    if (entry->d_type == DT_DIR) {
      // Hanya bersihkan folder hasil record EKG (dimulai dengan "ecg_archive")
      if (strncmp(entry->d_name, "ecg_archive", 11) == 0) {
        DIR *subdir = opendir(path);
        if (subdir) {
          struct dirent *subentry;
          while ((subentry = readdir(subdir)) != NULL) {
            if (subentry->d_type == DT_REG) {
              char filepath[768];
              snprintf(filepath, sizeof(filepath), "%s/%s", path, subentry->d_name);
              unlink(filepath);
              deleted_any = true;
            }
          }
          closedir(subdir);
        }
        rmdir(path);
        deleted_any = true;
      }
    } else if (entry->d_type == DT_REG) {
      // Hapus file reguler di root /data jika ada
      unlink(path);
      deleted_any = true;
    }
  }
  closedir(dir);

  if (deleted_any) {
    ESP_LOGI("boot_clean", "Storage telah dibersihkan dari file sisa sebelumnya.");
  } else {
    ESP_LOGI("boot_clean", "Tidak ada sisa file lama, storage bersih.");
  }

  // Unmount dan expose kembali ke USB Host agar siap digunakan PC/Mesin EKG
  storage_manager_expose_to_usb();
}

/* ─────────────────────────────────────────────────────────────
 * app_main — Entry Point
 * ───────────────────────────────────────────────────────────── */
void app_main(void) {
  /* Tandai firmware ini sebagai valid — mencegah rollback otomatis oleh bootloader.
   * PENTING: Harus dipanggil secepatnya setelah booting agar jika ada crash di
   * titik ini, bootloader TIDAK rollback ke firmware lama (menjaga loop update aman). */
  esp_ota_mark_app_valid_cancel_rollback();

  /* Mulai LED Indicator Task paling awal agar LED langsung mati sejak awal booting */
  xTaskCreate(led_task, "led_task", 2048, NULL, 3, NULL);

  /* Cek dan hapus sisa file rekaman EKG jika ada sebelum delay 5 detik */
  clean_storage_on_boot();

  ESP_LOGI(TAG, "Menunggu 5 detik sebelum memulai semua proses...");
  vTaskDelay(pdMS_TO_TICKS(5000));

  ESP_LOGI(TAG, "=== ECG Dongle Booting — Firmware v%s ===", ota_manager_get_current_version());

  /* Mulai task pemantau tombol */
  xTaskCreate(reset_btn_task, "reset_btn", 4096, NULL, 5, NULL);
  xTaskCreate(boot_btn_task, "boot_btn", 4096, NULL, 5, NULL);

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
    enter_ap_config_mode(false);
    return;
  }

  /* 2. Coba koneksi WiFi (timeout 30 detik) */
  ESP_LOGI(TAG, "Menghubungkan ke WiFi SSID: '%s' ...", s_wifi_ssid);
  s_led_state = LED_WIFI_CONNECTING;
  ret = wifi_manager_start(s_wifi_ssid, s_wifi_pass, 30000);

  if (ret != ESP_OK) {
    s_led_state = LED_ERROR_WIFI;
    vTaskDelay(pdMS_TO_TICKS(4000)); // Tampilkan error pattern lebih lama (4 detik) sebelum beralih
    /*
     * Koneksi WiFi GAGAL — masuk Mode AP agar pengguna
     * bisa memasukkan konfigurasi baru melalui browser.
     */
    ESP_LOGW(TAG, "WiFi gagal terkoneksi (%s). Beralih ke Mode AP...",
             esp_err_to_name(ret));
    enter_ap_config_mode(true);
    return;
  }

  /* WiFi OK — boot normal */
  ESP_LOGI(TAG, "WiFi terhubung! Melanjutkan proses normal...");
  esp_wifi_set_mode(WIFI_MODE_STA);
  run_normal_operation();
}

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console_cmd.h"
#include "dns_server.h"
#include "mdns.h"
#include "sdmmc_cmd.h"
#include "storage_manager.h"
#include "uploader.h"
#include "web_server.h"
#include "wifi_manager.h"

#define RESET_BTN_PIN GPIO_NUM_1
#define BOOT_BTN_PIN GPIO_NUM_0
#define LED_PIN GPIO_NUM_48 ///< Indikator status upload

/* ─────────────────────────────────────────────────────────────
 * LED Indicator Helpers
 * State:
 *   OFF          = Idle / Normal
 *   BLINK LAMBAT = Menunggu Idle timeout (safeguard 30 detik)
 *   BLINK CEPAT  = Sedang upload ke FTP
 *   ON SOLID     = Upload selesai (2 detik), lalu kembali OFF
 * ───────────────────────────────────────────────────────────── */
typedef enum {
  LED_OFF = 0,
  LED_BLINK_SLOW, // 1 Hz — fase safeguard
  LED_BLINK_FAST, // 5 Hz — fase upload
  LED_ON_SOLID,   // menyala penuh
} led_state_t;

static volatile led_state_t s_led_state = LED_OFF;

static void led_task(void *arg) {
  gpio_config_t io = {
      .pin_bit_mask = (1ULL << LED_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);
  gpio_set_level(LED_PIN, 0);

  while (1) {
    switch (s_led_state) {
    case LED_OFF:
      gpio_set_level(LED_PIN, 0);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;
    case LED_BLINK_SLOW:
      gpio_set_level(LED_PIN, 1);
      vTaskDelay(pdMS_TO_TICKS(500));
      gpio_set_level(LED_PIN, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      break;
    case LED_BLINK_FAST:
      gpio_set_level(LED_PIN, 1);
      vTaskDelay(pdMS_TO_TICKS(100));
      gpio_set_level(LED_PIN, 0);
      vTaskDelay(pdMS_TO_TICKS(100));
      break;
    case LED_ON_SOLID:
      gpio_set_level(LED_PIN, 1);
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
 *  3. Jika waktu sejak aktivitas terakhir mencapai 15 detik (Safeguard),
 *     maka bisa dipastikan mesin EKG benar-benar telah selesai.
 *  4. Ambil alih USB, eksekusi Upload.
 * ───────────────────────────────────────────────────────────── */
#define IDLE_SAFEGUARD_MS 15000 // Jeda aman 15 detik setelah write terakhir
#define IDLE_POLL_MS 500        // Cek setiap 500ms

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
                   "Transfer data sedang berlangsung... Upload otomatis akan terpicu jika mesin hening total selama %d detik.",
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

        // Tunggu sampai storage dikembalikan ke USB Host setelah upload
        for (int w = 0; w < 300; w++) {
          vTaskDelay(pdMS_TO_TICKS(1000));
          if (storage_manager_in_use_by_usb())
            break;
        }

        s_led_state = LED_ON_SOLID;
        vTaskDelay(pdMS_TO_TICKS(2000));
        s_led_state = LED_OFF;
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

          // Siapkan dashboard portal
          web_server_set_dashboard_mode(true);
          dns_server_start();

          // Set durasi timeout (2 menit)
          g_ap_timeout_sec = 120;

          // Create task timeout
          xTaskCreate(ap_timeout_task, "ap_timeout", 2048, NULL, 5, NULL);
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

int32_t g_ap_timeout_sec = 120; // 2 minutes

static void ap_timeout_task(void *arg) {
  while (g_ap_timeout_sec > 0) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    g_ap_timeout_sec--;
  }
  ESP_LOGI("ap_timeout", "Waktu 5 menit habis. Mematikan AP mode...");
  web_server_set_dashboard_mode(false);
  dns_server_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
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

  /* 7. Jalankan Console REPL */
  ESP_ERROR_CHECK(console_cmd_init());

  /* 8. Mulai mDNS */
  ESP_LOGI(TAG, "Memulai mDNS service...");
  ESP_ERROR_CHECK(mdns_init());
  mdns_hostname_set("medlink-dongle");
  mdns_instance_name_set("MedLink Dongle ECG");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  ESP_LOGI(TAG, "mDNS aktif. Akses via: http://medlink-dongle.local");

  /* 9. Mulai Web Server dan DNS (Captive Portal Dashboard) */
  ESP_LOGI(TAG, "Memulai Web Server dan DNS (Captive Portal)...");
  ESP_ERROR_CHECK(web_server_start());
  web_server_set_dashboard_mode(
      true); /* Tampilkan dashboard.html saat APSTA aktif */
  ESP_ERROR_CHECK(dns_server_start());

  /* 10. Mulai task countdown AP Timeout */
  xTaskCreate(ap_timeout_task, "ap_timeout", 2048, NULL, 5, NULL);

  /* 11. Mulai LED Indicator task */
  xTaskCreate(led_task, "led_task", 2048, NULL, 3, NULL);

  /* 12. Mulai Idle Detector (auto-upload saat USB idle 30 detik) */
  xTaskCreate(idle_detector_task, "idle_det", 4096, NULL, 4, NULL);

  ESP_LOGI(TAG, "=== ECG Dongle Siap ===");
}

/* ─────────────────────────────────────────────────────────────
 * app_main — Entry Point
 * ───────────────────────────────────────────────────────────── */
void app_main(void) {
  ESP_LOGI(TAG, "=== ECG Dongle Booting ===");

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

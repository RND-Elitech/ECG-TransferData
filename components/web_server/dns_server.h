#pragma once

#include "esp_err.h"

/**
 * @brief Mulai DNS server sederhana untuk Captive Portal.
 *
 * Semua query DNS akan dijawab dengan IP 192.168.4.1 (IP AP default ESP32),
 * sehingga browser smartphone/laptop otomatis mendeteksi portal konfigurasi.
 *
 * Harus dipanggil SETELAH mode AP aktif (wifi_manager_start_ap).
 *
 * @return ESP_OK jika berhasil, error lainnya jika gagal
 */
esp_err_t dns_server_start(void);

/**
 * @brief Hentikan DNS server.
 */
void dns_server_stop(void);

#pragma once

#include "esp_err.h"

/**
 * @brief Inisialisasi dan mulai koneksi Wi-Fi (mode Station).
 *
 * Fungsi ini akan memblokir hingga koneksi berhasil atau timeout.
 *
 * @param ssid     SSID jaringan Wi-Fi
 * @param password Password jaringan Wi-Fi
 * @param timeout_ms Timeout dalam milidetik (0 = tunggu selamanya)
 * @return ESP_OK jika berhasil terhubung, ESP_ERR_TIMEOUT jika gagal
 */
esp_err_t wifi_manager_start(const char *ssid, const char *password, uint32_t timeout_ms);

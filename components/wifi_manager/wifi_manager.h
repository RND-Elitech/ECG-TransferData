#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Mengecek apakah WiFi saat ini terhubung (mendapatkan IP).
 * @return true jika terhubung, false jika terputus
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Inisialisasi dan mulai koneksi Wi-Fi (mode Station).
 *
 * Fungsi ini akan memblokir hingga koneksi berhasil atau timeout.
 *
 * @param ssid       SSID jaringan Wi-Fi
 * @param password   Password jaringan Wi-Fi
 * @param timeout_ms Timeout dalam milidetik (0 = tunggu selamanya)
 * @return ESP_OK jika berhasil terhubung, ESP_ERR_TIMEOUT jika gagal
 */
esp_err_t wifi_manager_start(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief Set konfigurasi Static IP sebelum memulai koneksi Wi-Fi.
 *        Jika ip_addr di-set NULL atau kosong, maka akan kembali menggunakan DHCP (Dynamic IP).
 * 
 * @param ip_addr  IP Address statis (contoh: "192.168.1.100")
 * @param gateway  Gateway IP (contoh: "192.168.1.1")
 * @param netmask  Subnet mask (contoh: "255.255.255.0")
 */
void wifi_manager_set_static_ip(const char *ip_addr, const char *gateway, const char *netmask);

/**
 * @brief Aktifkan ESP32 sebagai Access Point (AP mode).
 *
 * SSID: "ECG-Gateway-XXXX" (XXXX = 4 digit hex dari MAC address)
 * Password: tidak ada (open network agar mudah diakses)
 * IP AP: 192.168.4.1
 *
 * @return ESP_OK jika berhasil
 */
esp_err_t wifi_manager_start_ap(void);

/**
 * @brief Hentikan WiFi (baik STA maupun AP mode).
 */
void wifi_manager_stop(void);

#pragma once

#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Mengambil Serial Number (SN) perangkat yang unik.
 * 
 * Mekanisme:
 * 1. Mencari kunci "dongle_sn" di partisi NVS namespace "factory".
 *    (Bisa di-inject saat perakitan menggunakan nvs_partition_gen.py)
 * 2. Jika tidak ada di NVS, fallback otomatis membaca MAC Address (eFuse).
 *    Format MAC fallback: "Dongle-A1B2C3"
 * 
 * @param buffer Buffer string untuk menyimpan SN.
 * @param max_len Ukuran maksimum buffer.
 * @return ESP_OK jika berhasil.
 */
esp_err_t device_info_get_sn(char *buffer, size_t max_len);

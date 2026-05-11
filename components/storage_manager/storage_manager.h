#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Inisialisasi media penyimpanan (SPI Flash atau SDMMC).
 *
 * Memilih media berdasarkan konfigurasi Kconfig:
 *   - CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH  → internal SPI Flash
 *   - Selain itu                              → SDMMC
 *
 * @return ESP_OK jika berhasil
 */
esp_err_t storage_manager_init(void);

/**
 * @brief Mount storage ke filesystem aplikasi (/data).
 *
 * @return ESP_OK jika berhasil
 */
esp_err_t storage_manager_mount(void);

/**
 * @brief Unmount storage dari filesystem aplikasi dan serahkan ke USB Host.
 *
 * @return ESP_OK jika berhasil
 */
esp_err_t storage_manager_expose_to_usb(void);

/**
 * @brief Hapus semua file & folder di storage (dipanggil sekali saat boot).
 */
void storage_manager_cleanup(void);

/**
 * @brief Cek apakah storage sedang dipakai oleh USB Host.
 *
 * @return true jika sedang di-expose ke USB Host
 */
bool storage_manager_in_use_by_usb(void);

uint32_t storage_manager_get_sector_count(void);
uint32_t storage_manager_get_sector_size(void);

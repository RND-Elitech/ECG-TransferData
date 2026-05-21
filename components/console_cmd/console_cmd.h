#pragma once

#include "esp_err.h"

/**
 * @brief Inisialisasi dan jalankan Console REPL (UART).
 *
 * Mendaftarkan semua command (upload, check, size, mount, expose, status)
 * dan memulai REPL task.
 *
 * @return ESP_OK jika berhasil
 */
esp_err_t console_cmd_init(void);

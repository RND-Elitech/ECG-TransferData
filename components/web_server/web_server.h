#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Mulai HTTP Web Server untuk konfigurasi portal AP.
 *
 * Menyajikan root.html, endpoint /scan untuk scan WiFi,
 * endpoint /save untuk menyimpan konfigurasi ke NVS dan restart.
 * Semua URI tidak dikenal di-redirect ke "/" (Captive Portal).
 *
 * @return ESP_OK jika berhasil, error code lain jika gagal
 */
esp_err_t web_server_start(void);

/**
 * @brief Hentikan HTTP Web Server.
 */
void web_server_stop(void);

/**
 * @brief Set mode tampilan web server.
 *
 * Jika enable=true, endpoint "/" akan menyajikan dashboard.html (AP Timer & Danger Zone).
 * Jika enable=false, endpoint "/" menyajikan root.html (Setup Portal biasa).
 *
 * @param enable true = dashboard mode, false = setup mode
 */
void web_server_set_dashboard_mode(bool enable);

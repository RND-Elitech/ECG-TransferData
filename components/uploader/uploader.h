#pragma once

#include "esp_err.h"

/**
 * @brief Konfigurasi untuk modul uploader.
 */
typedef struct {
    const char *ftp_host;     ///< Host/IP FTP server
    int ftp_port;             ///< Port FTP server (biasanya 21)
    const char *ftp_user;     ///< Username FTP
    const char *ftp_pass;     ///< Password FTP
    const char *base_path;    ///< Base path storage (contoh: "/data")
} uploader_config_t;

/**
 * @brief Inisialisasi modul uploader dengan konfigurasi server.
 *
 * @param cfg Pointer ke konfigurasi uploader
 */
void uploader_init(const uploader_config_t *cfg);

/**
 * @brief Mulai proses upload semua file ECG yang valid.
 *
 * Fungsi ini akan:
 *   1. Mount storage (jika sedang di-expose ke USB Host)
 *   2. Mencari folder ecg_archive terbaru
 *   3. Mengunggah semua file (.xml, .jpg, .bmp) secara berurutan
 *   4. Menghapus file yang berhasil diupload
 *   5. Mengembalikan storage ke USB Host
 *
 * @return ESP_OK jika semua file berhasil diupload, ESP_FAIL jika ada yang gagal
 */
esp_err_t uploader_run(void);

/**
 * @brief Callback yang digunakan oleh mqtt_manager sebagai handler perintah upload.
 *
 * Fungsi ini akan menjalankan uploader_run() di background FreeRTOS task.
 *
 * @param ctx Konteks (harus berupa pointer ke mqtt_manager_publish_upload_status)
 */
void uploader_trigger(void *ctx);

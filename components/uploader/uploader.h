#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Custom Error Codes untuk modul Uploader
#define UPLOADER_ERR_FTP_CONN   0x101
#define UPLOADER_ERR_FTP_LOGIN  0x102
#define UPLOADER_ERR_TRANSFER   0x103
#define UPLOADER_ERR_NO_FILES   0x104

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
 * @brief Trigger upload secara manual (mis. dari idle detector atau tombol).
 *
 * Fungsi ini akan menjalankan uploader_run() di background FreeRTOS task.
 *
 * @param ctx Tidak digunakan, bisa NULL
 */
void uploader_trigger(void *ctx);

/**
 * @brief Mengecek apakah proses upload sedang berjalan.
 * @return true jika sibuk, false jika selesai
 */
bool uploader_is_busy(void);

/**
 * @brief Mendapatkan status eksekusi upload terakhir.
 * @return ESP_OK, UPLOADER_ERR_FTP_CONN, UPLOADER_ERR_FTP_LOGIN, atau UPLOADER_ERR_TRANSFER
 */
esp_err_t uploader_get_last_status(void);

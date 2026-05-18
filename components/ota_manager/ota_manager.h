#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* ─── Versi firmware saat ini ─── */
#define APP_VERSION "1.0.0"

/* ─── URL default untuk cek pembaruan ─── */
/* Ganti dengan URL GitHub Releases atau server Anda */
#define OTA_VERSION_URL "https://raw.githubusercontent.com/rivaldisinkoprima/ECG-TransferData/main/version.json"
#define OTA_TIMEOUT_MS  15000  /* Timeout fetch version.json (15 detik) */

/* ─── Struktur hasil pengecekan versi ─── */
typedef struct {
    char current_version[16];   /**< Versi firmware yang sedang berjalan */
    char latest_version[16];    /**< Versi terbaru di server */
    bool update_available;      /**< true jika latest > current */
    char firmware_url[512];     /**< URL download firmware.bin */
    char release_notes[256];    /**< Catatan rilis */
} ota_check_result_t;

/* ─── Status proses OTA ─── */
typedef enum {
    OTA_STATE_IDLE,         /**< Tidak ada proses OTA */
    OTA_STATE_CHECKING,     /**< Sedang fetch version.json */
    OTA_STATE_DOWNLOADING,  /**< Sedang download & flash firmware */
    OTA_STATE_SUCCESS,      /**< OTA berhasil, menunggu reboot */
    OTA_STATE_FAILED,       /**< OTA gagal */
} ota_state_t;

/**
 * @brief Fetch version.json dari server dan bandingkan dengan versi saat ini.
 *        Fungsi ini blocking dan aman dipanggil dari HTTP handler.
 *
 * @param result  Pointer ke struct yang akan diisi dengan hasil pengecekan.
 * @return ESP_OK jika berhasil fetch dan parse, error code jika gagal.
 */
esp_err_t ota_manager_check(ota_check_result_t *result);

/**
 * @brief Mulai proses download dan flash firmware baru di background task.
 *        Perangkat akan reboot otomatis jika berhasil.
 *
 * @param firmware_url  URL lengkap ke file .bin firmware baru.
 * @return ESP_OK jika task berhasil dibuat, error jika OTA sedang berjalan.
 */
esp_err_t ota_manager_start_update(const char *firmware_url);

/**
 * @brief Dapatkan status proses OTA saat ini.
 */
ota_state_t ota_manager_get_state(void);

/**
 * @brief Dapatkan progress download saat ini (0-100).
 */
int ota_manager_get_progress(void);

/**
 * @brief Dapatkan versi firmware yang sedang berjalan.
 */
const char *ota_manager_get_current_version(void);

/**
 * @brief Dapatkan pesan error terakhir jika state == OTA_STATE_FAILED.
 */
const char *ota_manager_get_error_message(void);

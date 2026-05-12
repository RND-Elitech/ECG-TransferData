#include "uploader.h"
#include "storage_manager.h"
#include "mqtt_manager.h"

#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_log.h"
#include "FtpClient.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uploader";

/* State internal modul */
static uploader_config_t s_cfg = {0};

/* ─── Helper: tentukan MIME type berdasarkan ekstensi ─── */
static const char *_get_mime_type(const char *filename)
{
    if (strstr(filename, ".xml") || strstr(filename, ".XML")) return "application/xml";
    if (strstr(filename, ".jpg") || strstr(filename, ".JPG")) return "image/jpeg";
    if (strstr(filename, ".bmp") || strstr(filename, ".BMP")) return "image/bmp";
    return "application/octet-stream";
}

/* ─── Helper: cek apakah file adalah file ECG yang valid ─── */
static bool _is_valid_ecg_file(const char *name)
{
    return strstr(name, ".xml") || strstr(name, ".XML")
        || strstr(name, ".jpg") || strstr(name, ".JPG")
        || strstr(name, ".bmp") || strstr(name, ".BMP");
}

/* ─── Public API ─── */

void uploader_init(const uploader_config_t *cfg)
{
    if (cfg) {
        s_cfg = *cfg;
        ESP_LOGI(TAG, "Uploader siap → FTP %s:%d", cfg->ftp_host, cfg->ftp_port);
    }
}

esp_err_t uploader_run(void)
{
    bool needs_remount = storage_manager_in_use_by_usb();
    if (needs_remount) {
        ESP_LOGI(TAG, "Storage sedang di USB, mengambil alih...");
        if (storage_manager_mount() != ESP_OK) {
            ESP_LOGE(TAG, "Gagal mount storage");
            return ESP_FAIL;
        }
    }

    /* Cari folder ecg_archive terbaru */
    DIR *dir = opendir(s_cfg.base_path);
    if (!dir) {
        ESP_LOGE(TAG, "Gagal membuka direktori: %s", s_cfg.base_path);
        if (needs_remount) storage_manager_expose_to_usb();
        return ESP_FAIL;
    }

    struct dirent *entry;
    char latest_folder[256] = "";
    int max_index = -1;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_DIR) continue;
        if (strncmp(entry->d_name, "ecg_archive", 11) != 0) continue;

        if (strcmp(entry->d_name, "ecg_archive") == 0) {
            if (max_index < 0) { strcpy(latest_folder, entry->d_name); max_index = 0; }
        } else {
            int idx = atoi(entry->d_name + 11);
            if (idx > max_index) { max_index = idx; strcpy(latest_folder, entry->d_name); }
        }
    }
    closedir(dir);

    if (strlen(latest_folder) == 0) {
        ESP_LOGE(TAG, "Tidak ada folder ecg_archive di %s", s_cfg.base_path);
        if (needs_remount) storage_manager_expose_to_usb();
        return ESP_FAIL;
    }

    /* Upload semua file di folder tersebut */
    char folder_path[512];
    snprintf(folder_path, sizeof(folder_path), "%s/%s", s_cfg.base_path, latest_folder);
    DIR *subdir = opendir(folder_path);
    if (!subdir) {
        if (needs_remount) storage_manager_expose_to_usb();
        return ESP_FAIL;
    }

    /* Mulai Koneksi FTP */
    FtpClient* ftp = getFtpClient();
    NetBuf_t* ftp_ctrl = NULL;

    ESP_LOGI(TAG, "Menghubungkan ke FTP %s:%d", s_cfg.ftp_host, s_cfg.ftp_port);
    if (ftp->ftpClientConnect(s_cfg.ftp_host, s_cfg.ftp_port, &ftp_ctrl) != 1) {
        ESP_LOGE(TAG, "Koneksi FTP gagal");
        closedir(subdir);
        if (needs_remount) storage_manager_expose_to_usb();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Login FTP menggunakan %s...", s_cfg.ftp_user);
    if (ftp->ftpClientLogin(s_cfg.ftp_user, s_cfg.ftp_pass, ftp_ctrl) != 1) {
        ESP_LOGE(TAG, "Login FTP gagal");
        ftp->ftpClientQuit(ftp_ctrl);
        closedir(subdir);
        if (needs_remount) storage_manager_expose_to_usb();
        return ESP_FAIL;
    }

    int success = 0, fail = 0;
    bool found  = false;

    while ((entry = readdir(subdir)) != NULL) {
        if (entry->d_type != DT_REG || !_is_valid_ecg_file(entry->d_name)) continue;
        found = true;
        
        char file_path[768];
        snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, entry->d_name);
        
        ESP_LOGI(TAG, "Mengunggah via FTP: %s", entry->d_name);
        if (ftp->ftpClientPut(file_path, entry->d_name, FTP_CLIENT_BINARY, ftp_ctrl) == 1) {
            ESP_LOGI(TAG, "Upload FTP sukses, hapus file: %s", file_path);
            unlink(file_path);
            success++;
        } else {
            ESP_LOGE(TAG, "Upload FTP gagal untuk file: %s", file_path);
            fail++;
        }
    }
    closedir(subdir);

    ftp->ftpClientQuit(ftp_ctrl);

    if (!found) {
        ESP_LOGE(TAG, "Tidak ada file valid di %s", folder_path);
        if (needs_remount) storage_manager_expose_to_usb();
        return ESP_FAIL;
    }

    if (success > 0 && fail == 0) {
        ESP_LOGI(TAG, "Semua %d file terupload. Hapus folder %s", success, folder_path);
        rmdir(folder_path);
    } else {
        ESP_LOGW(TAG, "Selesai. Sukses: %d, Gagal: %d", success, fail);
    }

    if (needs_remount) {
        ESP_LOGI(TAG, "Kembalikan storage ke USB Host...");
        storage_manager_expose_to_usb();
    }

    return (fail == 0) ? ESP_OK : ESP_FAIL;
}

/* ─── Background task wrapper ─── */
static void _upload_task(void *pvParameters)
{
    esp_err_t ret = uploader_run();
    mqtt_manager_publish_upload_status(ret == ESP_OK);
    vTaskDelete(NULL);
}

void uploader_trigger(void *ctx)
{
    ESP_LOGI(TAG, "Memulai background upload task...");
    xTaskCreate(_upload_task, "uploader_task", 8192, NULL, 5, NULL);
}

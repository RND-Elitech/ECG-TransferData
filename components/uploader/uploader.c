#include "uploader.h"
#include "storage_manager.h"

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
static volatile bool g_is_uploading = false;
static volatile esp_err_t s_last_upload_status = ESP_OK;

bool uploader_is_busy(void) { return g_is_uploading; }
esp_err_t uploader_get_last_status(void) { return s_last_upload_status; }

/* ─── Helper: cek apakah file adalah file ECG yang valid ─── */
static bool _is_valid_ecg_file(const char *name)
{
    return strstr(name, ".xml") || strstr(name, ".XML")
        || strstr(name, ".jpg") || strstr(name, ".JPG")
        || strstr(name, ".bmp") || strstr(name, ".BMP");
}

/* ─── Public API ─── */

static void _force_delete_folder(const char *folder_path) {
    DIR *d = opendir(folder_path);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_type == DT_REG) {
            char file_path[768];
            snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, entry->d_name);
            unlink(file_path);
        }
    }
    closedir(d);
    rmdir(folder_path);
    ESP_LOGI("uploader", "Folder %s dan isinya dihapus agar bisa coba lagi.", folder_path);
}

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
        return UPLOADER_ERR_NO_FILES;
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
        return UPLOADER_ERR_NO_FILES;
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

    int success = 0, fail = 0;
    bool found  = false;

    ESP_LOGI(TAG, "--- Debug FTP Configuration ---");
    ESP_LOGI(TAG, "Host     : [%s]", s_cfg.ftp_host);
    ESP_LOGI(TAG, "Port     : %d", s_cfg.ftp_port);
    ESP_LOGI(TAG, "Username : [%s]", s_cfg.ftp_user);
    ESP_LOGI(TAG, "Password : [%s]", s_cfg.ftp_pass);
    ESP_LOGI(TAG, "-------------------------------");

    ESP_LOGI(TAG, "Menghubungkan ke FTP %s:%d", s_cfg.ftp_host, s_cfg.ftp_port);
    if (ftp->ftpClientConnect(s_cfg.ftp_host, s_cfg.ftp_port, &ftp_ctrl) != 1) {
        ESP_LOGE(TAG, "Koneksi FTP gagal (Mungkin Host kosong atau tidak terjangkau)");
        closedir(subdir);
        _force_delete_folder(folder_path); // Hapus file yang tertahan
        if (needs_remount) storage_manager_expose_to_usb();
        return UPLOADER_ERR_FTP_CONN;
    }

    ESP_LOGI(TAG, "Login FTP menggunakan %s...", s_cfg.ftp_user);
    if (ftp->ftpClientLogin(s_cfg.ftp_user, s_cfg.ftp_pass, ftp_ctrl) != 1) {
        ESP_LOGE(TAG, "Login FTP gagal untuk user: %s", s_cfg.ftp_user);
        ftp->ftpClientQuit(ftp_ctrl);
        closedir(subdir);
        _force_delete_folder(folder_path); // Hapus file yang tertahan
        if (needs_remount) storage_manager_expose_to_usb();
        return UPLOADER_ERR_FTP_LOGIN;
    }

    while ((entry = readdir(subdir)) != NULL) {
        if (entry->d_type != DT_REG || !_is_valid_ecg_file(entry->d_name)) continue;
        found = true;
        
        char file_path[768];
        snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, entry->d_name);
        
        ESP_LOGI(TAG, "Mengunggah via FTP: %s", entry->d_name);
        if (ftp->ftpClientPut(file_path, entry->d_name, FTP_CLIENT_BINARY, ftp_ctrl) == 1) {
            ESP_LOGI(TAG, "Upload FTP sukses, hapus file: %s", file_path);
            success++;
        } else {
            ESP_LOGE(TAG, "Upload FTP gagal untuk file: %s", file_path);
            fail++;
        }
        unlink(file_path); // Selalu hapus file, baik upload sukses maupun gagal
    }
    closedir(subdir);

    ftp->ftpClientQuit(ftp_ctrl);

    if (!found) {
        ESP_LOGW(TAG, "Tidak ada file valid di %s, menghapus folder kosong.", folder_path);
        rmdir(folder_path);
        if (needs_remount) storage_manager_expose_to_usb();
        return UPLOADER_ERR_NO_FILES;
    }

    if (success > 0 || fail > 0) {
        ESP_LOGI(TAG, "Proses selesai. Sukses: %d, Gagal: %d. Folder %s akan dihapus.", success, fail, folder_path);
        rmdir(folder_path);
    }

    if (needs_remount) {
        ESP_LOGI(TAG, "Kembalikan storage ke USB Host...");
        storage_manager_expose_to_usb();
    }

    return (fail == 0) ? ESP_OK : UPLOADER_ERR_TRANSFER;
}

/* ─── Background task wrapper ─── */
static void _upload_task(void *pvParameters)
{
    g_is_uploading = true;
    esp_err_t ret = uploader_run();
    s_last_upload_status = ret;
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Upload selesai: semua file berhasil dikirim.");
    } else {
        ESP_LOGW(TAG, "Upload selesai: ada file yang gagal dikirim.");
    }
    g_is_uploading = false;
    vTaskDelete(NULL);
}

void uploader_trigger(void *ctx)
{
    ESP_LOGI(TAG, "Memulai background upload task...");
    xTaskCreate(_upload_task, "uploader_task", 8192, NULL, 5, NULL);
}

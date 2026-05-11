#include "uploader.h"
#include "storage_manager.h"
#include "mqtt_manager.h"

#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_http_client.h"
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

/* ─── Upload satu file ke server ─── */
static esp_err_t _upload_file(const char *folder_path, const char *filename)
{
    char file_path[768];
    snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, filename);

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Gagal membuka file: %s", file_path);
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < 10) {
        ESP_LOGE(TAG, "File terlalu kecil: %s (%zu bytes)", file_path, file_size);
        fclose(fp);
        return ESP_FAIL;
    }

    const char *boundary = "----ESP32FormBoundary123456";
    const char *mime     = _get_mime_type(filename);

    char header[512], footer[64];
    snprintf(header, sizeof(header),
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n",
        boundary, filename, mime);
    snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    size_t total_len = strlen(header) + file_size + strlen(footer);

    esp_http_client_config_t http_cfg = {
        .url        = s_cfg.server_url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) { fclose(fp); return ESP_FAIL; }

    char content_type[128];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open gagal: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(fp);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Mengunggah: %s (%zu bytes)", filename, file_size);

    /* Kirim header multipart */
    esp_http_client_write(client, header, strlen(header));

    /* Kirim isi file */
    uint8_t buf[1024];
    size_t sent = 0;
    while (sent < file_size) {
        size_t to_read = (file_size - sent < sizeof(buf)) ? (file_size - sent) : sizeof(buf);
        size_t n = fread(buf, 1, to_read, fp);
        if (n <= 0) { err = ESP_FAIL; break; }
        if (esp_http_client_write(client, (char *)buf, n) < 0) { err = ESP_FAIL; break; }
        sent += n;
    }
    fclose(fp);

    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Kirim footer multipart */
    esp_http_client_write(client, footer, strlen(footer));

    /* Ambil respons */
    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d", status_code);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code == 200 || status_code == 201) {
        ESP_LOGI(TAG, "Upload sukses, hapus file: %s", file_path);
        unlink(file_path);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Upload gagal dengan HTTP status %d", status_code);
    return ESP_FAIL;
}

/* ─── Public API ─── */

void uploader_init(const uploader_config_t *cfg)
{
    s_cfg = *cfg;
    ESP_LOGI(TAG, "Uploader siap → %s", cfg->server_url);
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

    int success = 0, fail = 0;
    bool found  = false;

    while ((entry = readdir(subdir)) != NULL) {
        if (entry->d_type != DT_REG || !_is_valid_ecg_file(entry->d_name)) continue;
        found = true;
        if (_upload_file(folder_path, entry->d_name) == ESP_OK) success++;
        else fail++;
    }
    closedir(subdir);

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

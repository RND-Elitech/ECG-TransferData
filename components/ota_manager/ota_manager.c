#include "ota_manager.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_manager";

/* ─── State internal ─── */
static volatile ota_state_t  s_state    = OTA_STATE_IDLE;
static volatile int          s_progress = 0;
static char s_error_msg[128]            = "";
static char s_pending_url[512]          = "";

/* ─── HTTP response buffer untuk fetch version.json ─── */
#define HTTP_BUF_SIZE 2048
static char s_http_buf[HTTP_BUF_SIZE];
static int  s_http_buf_len = 0;

/* ────────────────────────────────────────────────
 * HTTP Event Handler — kumpulkan response body
 * ──────────────────────────────────────────────── */
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        s_http_buf_len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        {
            int copy_len = evt->data_len;
            if (s_http_buf_len + copy_len >= HTTP_BUF_SIZE) {
                copy_len = HTTP_BUF_SIZE - s_http_buf_len - 1;
            }
            if (copy_len > 0) {
                memcpy(s_http_buf + s_http_buf_len, evt->data, copy_len);
                s_http_buf_len += copy_len;
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Helper: Bandingkan versi "major.minor.patch"
 * Return: 1 jika a > b, -1 jika a < b, 0 jika sama
 * ──────────────────────────────────────────────── */
static int _compare_versions(const char *a, const char *b)
{
    int a1 = 0, a2 = 0, a3 = 0;
    int b1 = 0, b2 = 0, b3 = 0;
    sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b, "%d.%d.%d", &b1, &b2, &b3);

    if (a1 != b1) return (a1 > b1) ? 1 : -1;
    if (a2 != b2) return (a2 > b2) ? 1 : -1;
    if (a3 != b3) return (a3 > b3) ? 1 : -1;
    return 0;
}

/* ────────────────────────────────────────────────
 * OTA Download Task (berjalan di background)
 * ──────────────────────────────────────────────── */
static void _ota_update_task(void *pvParameter)
{
    const char *url = (const char *)pvParameter;
    /* State sudah di-set ke DOWNLOADING oleh ota_manager_start_update() sebelum
     * task ini dibuat — tidak perlu di-set lagi di sini agar ap_timeout_task
     * langsung melihat state yang benar tanpa race condition. */
    s_progress = 0;

    ESP_LOGI(TAG, "Memulai OTA download dari: %s", url);

    /* ── Tunggu sebentar agar WiFi/DNS stack fully ready setelah handoff ── */
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .timeout_ms        = 60000, /* Perbesar timeout: firmware bisa > 1MB */
        .keep_alive_enable = true,
        .buffer_size       = 4096,
        .buffer_size_tx    = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    /* ── Retry logic: coba hingga 3x jika koneksi gagal (DNS transient error) ── */
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = esp_https_ota_begin(&ota_cfg, &ota_handle);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "OTA begin gagal (percobaan %d/3): %s — tunggu 3 detik...",
                 attempt, esp_err_to_name(err));
        if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(3000));
    }

    if (err != ESP_OK) {
        snprintf(s_error_msg, sizeof(s_error_msg), "OTA begin gagal setelah 3x percobaan: %s",
                 esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_error_msg);
        s_state = OTA_STATE_FAILED;
        vTaskDelete(NULL);
        return;
    }

    /* Baca app desc untuk validasi firmware sebelum flash */
    esp_app_desc_t new_app_desc;
    if (esp_https_ota_get_img_desc(ota_handle, &new_app_desc) == ESP_OK) {
        ESP_LOGI(TAG, "Firmware baru: project='%s' version='%s'",
                 new_app_desc.project_name, new_app_desc.version);
    }

    /* Ambil total ukuran file dari HTTP Content-Length (jika didukung server) */
    int total_size = esp_https_ota_get_image_size(ota_handle);
    
    /* Loop download dan flash dengan progress tracking */
    int bytes_written = 0;
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            bytes_written = esp_https_ota_get_image_len_read(ota_handle);
            
            int estimated = 0;
            if (total_size > 0) {
                /* Hitung persentase berdasarkan ukuran file asli */
                estimated = (bytes_written * 100) / total_size;
            } else {
                /* Fallback jika server tidak mengirim Content-Length */
                estimated = bytes_written / 30000; /* ~1% per 30KB */
            }
            
            s_progress = (estimated > 99) ? 99 : estimated;
            continue;
        }
        break;
    }
    (void)bytes_written; /* suppress unused warning jika loop langsung selesai */

    if (err != ESP_OK) {
        snprintf(s_error_msg, sizeof(s_error_msg), "OTA perform gagal: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_error_msg);
        esp_https_ota_abort(ota_handle);
        s_state = OTA_STATE_FAILED;
        vTaskDelete(NULL);
        return;
    }

    /* Finalisasi dan tandai firmware baru sebagai pending */
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        snprintf(s_error_msg, sizeof(s_error_msg), "OTA finish gagal: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_error_msg);
        s_state = OTA_STATE_FAILED;
        vTaskDelete(NULL);
        return;
    }

    s_progress = 100;
    s_state    = OTA_STATE_SUCCESS;
    ESP_LOGI(TAG, "OTA berhasil! Reboot dalam 3 detik...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    vTaskDelete(NULL);
}

/* ────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────── */

esp_err_t ota_manager_check(ota_check_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;

    s_state = OTA_STATE_CHECKING;
    memset(result, 0, sizeof(ota_check_result_t));
    strncpy(result->current_version, APP_VERSION, sizeof(result->current_version) - 1);

    /* Fetch version.json */
    s_http_buf_len = 0;
    esp_http_client_config_t http_cfg = {
        .url              = OTA_VERSION_URL,
        .timeout_ms       = OTA_TIMEOUT_MS,
        .buffer_size      = 4096, /* WAJIB BESAR UNTUK GITHUB (Header Panjang) */
        .buffer_size_tx   = 1024,
        .event_handler    = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach, /* Gunakan global certificate bundle */
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        s_state = OTA_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }

    /* ── Supabase membutuhkan dua header untuk autentikasi anon ── */
    esp_http_client_set_header(client, "apikey",        SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", "Bearer " SUPABASE_ANON_KEY);

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Gagal fetch version.json: err=%s, status=%d",
                 esp_err_to_name(err), status);
        s_state = OTA_STATE_IDLE;
        return ESP_FAIL;
    }

    s_http_buf[s_http_buf_len] = '\0';
    ESP_LOGI(TAG, "version.json: %s", s_http_buf);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) {
        ESP_LOGE(TAG, "Gagal parse REST API response");
        s_state = OTA_STATE_IDLE;
        return ESP_FAIL;
    }

    /* Supabase REST API (PostgREST) mengembalikan Array JSON, ambil elemen pertama */
    cJSON *row = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : root;
    if (!row) {
        ESP_LOGI(TAG, "Tidak ada pembaruan firmware aktif di Supabase.");
        cJSON_Delete(root);
        s_state = OTA_STATE_IDLE;
        /* Kembalikan OK tapi update_available = false */
        result->update_available = false;
        return ESP_OK;
    }

    cJSON *j_version = cJSON_GetObjectItem(row, "version");
    cJSON *j_url     = cJSON_GetObjectItem(row, "firmware_url");
    cJSON *j_notes   = cJSON_GetObjectItem(row, "release_notes");

    if (!cJSON_IsString(j_version) || !cJSON_IsString(j_url)) {
        ESP_LOGE(TAG, "Format data Supabase tidak valid (butuh 'version' dan 'firmware_url')");
        cJSON_Delete(root);
        s_state = OTA_STATE_IDLE;
        return ESP_FAIL;
    }

    strncpy(result->latest_version, j_version->valuestring, sizeof(result->latest_version) - 1);
    strncpy(result->firmware_url,   j_url->valuestring,     sizeof(result->firmware_url) - 1);
    if (cJSON_IsString(j_notes)) {
        strncpy(result->release_notes, j_notes->valuestring, sizeof(result->release_notes) - 1);
    }

    result->update_available = (_compare_versions(result->latest_version, result->current_version) > 0);
    cJSON_Delete(root);

    s_state = OTA_STATE_IDLE;
    ESP_LOGI(TAG, "Versi saat ini: %s | Versi terbaru: %s | Update: %s",
             result->current_version, result->latest_version,
             result->update_available ? "YA" : "Tidak");
    return ESP_OK;
}

esp_err_t ota_manager_start_update(const char *firmware_url)
{
    if (!firmware_url || strlen(firmware_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state == OTA_STATE_DOWNLOADING) {
        ESP_LOGW(TAG, "OTA sudah berjalan, abaikan request baru.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Simpan URL ke buffer statis agar task bisa akses */
    strncpy(s_pending_url, firmware_url, sizeof(s_pending_url) - 1);
    s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    s_progress = 0;
    s_error_msg[0] = '\0';

    /* ── Set state SEBELUM membuat task ──────────────────────────────────────
     * Ini KRITIS: ap_timeout_task cek state setiap detik. Jika state masih
     * IDLE saat task baru dibuat, ada race condition di mana ap_timeout_task
     * bisa mematikan AP/WiFi sebelum _ota_update_task sempat jalan.
     * ─────────────────────────────────────────────────────────────────────── */
    s_state = OTA_STATE_DOWNLOADING;

    BaseType_t ret = xTaskCreate(
        _ota_update_task,
        "ota_task",
        16384, /* Perbesar stack: HTTPS OTA + TLS butuh minimal 12KB */
        (void *)s_pending_url,
        5,
        NULL
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat ota_task");
        s_state = OTA_STATE_FAILED; /* Reset state jika task gagal dibuat */
        return ESP_FAIL;
    }

    return ESP_OK;
}

ota_state_t ota_manager_get_state(void)        { return s_state; }
int         ota_manager_get_progress(void)      { return s_progress; }
const char *ota_manager_get_current_version(void) { return APP_VERSION; }
const char *ota_manager_get_error_message(void) { return s_error_msg; }

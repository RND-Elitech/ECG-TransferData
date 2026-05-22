/**
 * @file web_server.c
 * @brief HTTP Web Server untuk Captive Portal / Configuration Portal ECG Gateway
 *
 * Endpoint:
 *  GET  /         -> Sajikan root.html (embedded)
 *  GET  /scan     -> Scan WiFi, return JSON [{ssid, rssi}, ...]
 *  POST /save     -> Simpan konfigurasi ke NVS, restart ESP32
 *  GET  *         -> Redirect ke "/" (Captive Portal)
 *
 * Captive portal hooks (Android / iOS / Windows):
 *  GET /generate_204      -> 204 No Content (Android)
 *  GET /connecttest.txt   -> 200 "Microsoft NCSI" (Windows)
 *  GET /hotspot-detect.html -> redirect (iOS/macOS)
 *  GET /library/test/success.html -> redirect (iOS)
 */

#include "web_server.h"

#include <string.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "cJSON.h"
#include "ota_manager.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;
static bool s_is_dashboard = false;
extern int32_t g_ap_timeout_sec;

/* ─── Embedded HTML files ─── */
extern const uint8_t root_html_start[]      asm("_binary_root_html_start");
extern const uint8_t root_html_end[]        asm("_binary_root_html_end");
extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[]   asm("_binary_dashboard_html_end");

/* ────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────── */

/**
 * @brief URL-decode in-place. Returns pointer to buf.
 *        %XX -> char, '+' -> ' '
 */
static char *url_decode(char *buf) {
    char *p = buf, *q = buf;
    while (*p) {
        if (*p == '%' && *(p+1) && *(p+2)) {
            char hex[3] = {*(p+1), *(p+2), 0};
            *q++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else if (*p == '+') {
            *q++ = ' ';
            p++;
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
    return buf;
}

/**
 * @brief Cari value dari form field "key=value&..." di body.
 *        Result disalin ke out_buf (max out_len bytes, null-terminated).
 */
static bool parse_form_field(const char *body, const char *key,
                             char *out_buf, size_t out_len) {
    /* Buat search pattern "key=" */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *found = strstr(body, pattern);
    if (!found) {
        out_buf[0] = '\0';
        return false;
    }
    found += strlen(pattern);
    const char *end = strchr(found, '&');
    size_t len = end ? (size_t)(end - found) : strlen(found);
    if (len >= out_len) len = out_len - 1;
    strncpy(out_buf, found, len);
    out_buf[len] = '\0';
    url_decode(out_buf);
    return (out_buf[0] != '\0');
}

/**
 * @brief NVS helper — simpan string
 */
static esp_err_t nvs_set_str_helper(nvs_handle_t h, const char *key, const char *val) {
    esp_err_t err = nvs_set_str(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str '%s' gagal: %s", key, esp_err_to_name(err));
    }
    return err;
}

/* ────────────────────────────────────────────────
 * Handler: GET /  (root.html or dashboard.html)
 * ──────────────────────────────────────────────── */
static esp_err_t handler_root(httpd_req_t *req) {
    const uint8_t *html_start;
    size_t html_len;

    if (s_is_dashboard) {
        html_start = dashboard_html_start;
        html_len   = dashboard_html_end - dashboard_html_start;
    } else {
        html_start = root_html_start;
        html_len   = root_html_end - root_html_start;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)html_start, (ssize_t)html_len);
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: GET /scan  (WiFi scan)
 * ──────────────────────────────────────────────── */
static esp_err_t handler_scan(httpd_req_t *req) {
    /* Mulai scan (blocking, max 5 AP terdekat) */
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    /* Build JSON array */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_list[i].rssi);
        cJSON_AddItemToArray(arr, item);
    }
    free(ap_list);

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str ? json_str : "[]");
    if (json_str) free(json_str);
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: POST /save  (Simpan konfigurasi + restart)
 * ──────────────────────────────────────────────── */
static esp_err_t handler_save(httpd_req_t *req) {
    /* Baca body (max 2 KB) */
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body size");
        return ESP_FAIL;
    }

    char *body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            free(body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        received += ret;
    }
    body[total] = '\0';
    ESP_LOGI(TAG, "POST /save body: %s", body);

    /* Parse semua field */
    char wifi_ssid[64] = {0}, wifi_pass[64] = {0};
    char ip_mode[16]   = {0};
    char ip_addr[20]   = {0}, subnet[20] = {0}, gateway[20] = {0};
    char ftp_host[64]  = {0}, ftp_port_s[8] = {0};
    char ftp_user[64]  = {0}, ftp_pass[64]  = {0};

    parse_form_field(body, "ssid",        wifi_ssid,  sizeof(wifi_ssid));
    parse_form_field(body, "password",    wifi_pass,  sizeof(wifi_pass));
    parse_form_field(body, "ip_mode",     ip_mode,    sizeof(ip_mode));
    parse_form_field(body, "ip_address",  ip_addr,    sizeof(ip_addr));
    parse_form_field(body, "subnet_mask", subnet,     sizeof(subnet));
    parse_form_field(body, "gateway",     gateway,    sizeof(gateway));

    /* Tangkap data FTP dari body (Jangan lupa!) */
    parse_form_field(body, "ftp_host",    ftp_host,   sizeof(ftp_host));
    parse_form_field(body, "ftp_port",    ftp_port_s, sizeof(ftp_port_s));
    parse_form_field(body, "ftp_user",    ftp_user,   sizeof(ftp_user));
    parse_form_field(body, "ftp_pass",    ftp_pass,   sizeof(ftp_pass));
    
    free(body);

    /* Validasi minimal */
    if (wifi_ssid[0] == '\0') {
        const char *resp = "{\"status\":\"error\",\"message\":\"WiFi Name (SSID) cannot be empty\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }
    if (strlen(wifi_pass) < 8) {
        const char *resp = "{\"status\":\"error\",\"message\":\"Password must be at least 8 characters\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    /* Uji koneksi WiFi sebelum menyimpan dan melakukan restart */
    ESP_LOGI(TAG, "Menguji koneksi WiFi ke SSID: '%s'...", wifi_ssid);
    
    if (strcmp(ip_mode, "static") == 0 && ip_addr[0] != '\0') {
        wifi_manager_set_static_ip(ip_addr, gateway, subnet);
    } else {
        wifi_manager_set_static_ip(NULL, NULL, NULL);
    }

    // Gunakan timeout 15 detik (15000 ms) untuk pengujian
    esp_err_t conn_err = wifi_manager_start(wifi_ssid, wifi_pass, 15000);
    
    if (conn_err != ESP_OK) {
        ESP_LOGE(TAG, "Uji koneksi WiFi gagal: %s", esp_err_to_name(conn_err));
        
        char resp_err[256];
        snprintf(resp_err, sizeof(resp_err), 
                 "{\"status\":\"error\",\"message\":\"Failed to connect to WiFi '%s'. Incorrect password or network not found.\"}", 
                 wifi_ssid);
        
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp_err);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Uji koneksi berhasil! Menyimpan konfigurasi ke NVS...");

    /* Simpan ke NVS */
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal membuka NVS: %s", esp_err_to_name(err));
        const char *resp = "{\"status\":\"error\",\"message\":\"Failed to save settings (NVS error)\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    nvs_set_str_helper(nvs_h, "wifi_ssid", wifi_ssid);
    nvs_set_str_helper(nvs_h, "wifi_pass", wifi_pass);
    nvs_set_str_helper(nvs_h, "ip_mode",   ip_mode[0] ? ip_mode : "dynamic");
    if (ip_addr[0])  nvs_set_str_helper(nvs_h, "ip_addr",  ip_addr);
    if (subnet[0])   nvs_set_str_helper(nvs_h, "ip_subnet", subnet);
    if (gateway[0])  nvs_set_str_helper(nvs_h, "ip_gw",    gateway);

    if (ftp_host[0]) nvs_set_str_helper(nvs_h, "ftp_host", ftp_host);
    if (ftp_port_s[0]) {
        int32_t ftp_port = (int32_t)atoi(ftp_port_s);
        nvs_set_i32(nvs_h, "ftp_port", ftp_port);
    }
    if (ftp_user[0]) nvs_set_str_helper(nvs_h, "ftp_user", ftp_user);
    if (ftp_pass[0]) nvs_set_str_helper(nvs_h, "ftp_pass", ftp_pass);

    nvs_commit(nvs_h);
    nvs_close(nvs_h);

    ESP_LOGI(TAG, "Konfigurasi tersimpan. Akan restart dalam 1 detik...");

    /* Kirim respon sukses sebelum restart */
    const char *resp_ok = "{\"status\":\"success\","
                          "\"message\":\"Configuration saved! The device will restart and connect to the WiFi network...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_ok);

    /* Delay singkat lalu restart */
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();

    return ESP_OK;
}



/* ────────────────────────────────────────────────
 * Handler: POST /reset  (Reset to factory settings)
 * ──────────────────────────────────────────────── */
static esp_err_t handler_reset(httpd_req_t *req) {
    /* Read body */
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body size");
        return ESP_FAIL;
    }

    char *body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[total] = '\0';

    char password[64] = {0};
    parse_form_field(body, "password", password, sizeof(password));
    free(body);

    if (strcmp(password, "admin") != 0) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid password");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Factory reset requested via Web Dashboard");
    
    nvs_handle_t nvs_h;
    if (nvs_open("config", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_erase_all(nvs_h);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
    
    httpd_resp_sendstr(req, "OK");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: GET /ap_status
 * ──────────────────────────────────────────────── */
static esp_err_t handler_ap_status(httpd_req_t *req) {
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"remaining_seconds\":%ld}", (long)g_ap_timeout_sec);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: POST /ap_extend
 * ──────────────────────────────────────────────── */
static esp_err_t handler_ap_extend(httpd_req_t *req) {
    if (g_ap_timeout_sec > 0) {
        g_ap_timeout_sec += 60;
        ESP_LOGI(TAG, "AP Timeout diperpanjang 60 detik");
    }
    const char *resp = "{\"status\":\"success\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: GET /ota/check
 * Fetch version.json dari server dan kembalikan hasil perbandingan versi.
 * ──────────────────────────────────────────────── */
static esp_err_t handler_ota_check(httpd_req_t *req) {
    ota_check_result_t result;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t err = ota_manager_check(&result);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "current_version", result.current_version);
    cJSON_AddStringToObject(root, "latest_version",  result.latest_version);
    cJSON_AddBoolToObject  (root, "update_available", result.update_available);
    cJSON_AddStringToObject(root, "firmware_url",    result.firmware_url);
    cJSON_AddStringToObject(root, "release_notes",   result.release_notes);

    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", "Gagal menghubungi server pembaruan.");
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_sendstr(req, json_str ? json_str : "{}");
    if (json_str) free(json_str);
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: POST /ota/start
 * Body JSON: { "firmware_url": "https://..." }
 * ──────────────────────────────────────────────── */
static esp_err_t handler_ota_start(httpd_req_t *req) {
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }

    char *body = malloc(total + 1);
    if (!body) { httpd_resp_send_500(req); return ESP_ERR_NO_MEM; }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) { free(body); return ESP_FAIL; }
        received += ret;
    }
    body[total] = '\0';

    cJSON *root  = cJSON_Parse(body);
    free(body);

    cJSON *j_url = root ? cJSON_GetObjectItem(root, "firmware_url") : NULL;
    if (!cJSON_IsString(j_url)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing firmware_url");
        return ESP_FAIL;
    }

    esp_err_t err = ota_manager_start_update(j_url->valuestring);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"started\"}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_sendstr(req, "{\"status\":\"already_running\"}");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\"}");
    }
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: GET /ota/progress
 * ──────────────────────────────────────────────── */
static esp_err_t handler_ota_progress(httpd_req_t *req) {
    const char *state_str;
    switch (ota_manager_get_state()) {
        case OTA_STATE_CHECKING:     state_str = "checking";     break;
        case OTA_STATE_DOWNLOADING:  state_str = "downloading";  break;
        case OTA_STATE_SUCCESS:      state_str = "success";      break;
        case OTA_STATE_FAILED:       state_str = "failed";       break;
        default:                     state_str = "idle";         break;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state",   state_str);
    cJSON_AddNumberToObject(root, "progress", ota_manager_get_progress());
    cJSON_AddStringToObject(root, "error",   ota_manager_get_error_message());

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str ? json_str : "{}");
    if (json_str) free(json_str);
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: Android captive portal probe  GET /generate_204
 * ──────────────────────────────────────────────── */
static esp_err_t handler_generate_204(httpd_req_t *req) {
    /* Redirect ke halaman config agar Android buka portal */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: Windows NCSI  GET /connecttest.txt
 * ──────────────────────────────────────────────── */
static esp_err_t handler_ncsi(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Microsoft NCSI");
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Handler: Catch-All (redirect ke /)
 * ──────────────────────────────────────────────── */
static esp_err_t handler_catchall(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────── */
esp_err_t web_server_start(void) {
    if (s_server) {
        ESP_LOGW(TAG, "Server sudah berjalan");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;
    config.stack_size = 10240; /* Tambah stack size untuk mencegah overflow saat OTA check (TLS/HTTPS) */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Memulai HTTP server...");
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    /* Daftarkan semua URI handler */
    static const httpd_uri_t uris[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = handler_root         },
        { .uri = "/scan",              .method = HTTP_GET,  .handler = handler_scan         },
        { .uri = "/save",              .method = HTTP_POST, .handler = handler_save         },
        { .uri = "/reset",             .method = HTTP_POST, .handler = handler_reset        },
        { .uri = "/ap_status",         .method = HTTP_GET,  .handler = handler_ap_status    },
        { .uri = "/ap_extend",         .method = HTTP_POST, .handler = handler_ap_extend    },
        { .uri = "/ota/check",         .method = HTTP_GET,  .handler = handler_ota_check    },
        { .uri = "/ota/start",         .method = HTTP_POST, .handler = handler_ota_start    },
        { .uri = "/ota/progress",      .method = HTTP_GET,  .handler = handler_ota_progress },
        { .uri = "/generate_204",      .method = HTTP_GET,  .handler = handler_generate_204 },
        { .uri = "/connecttest.txt",   .method = HTTP_GET,  .handler = handler_ncsi         },
        { .uri = "/hotspot-detect.html",.method = HTTP_GET, .handler = handler_catchall     },
        { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = handler_catchall },
        { .uri = "/*",                 .method = HTTP_GET,  .handler = handler_catchall     },
    };

    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server berjalan di port 80");
    return ESP_OK;
}

void web_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server dihentikan");
    }
}

void web_server_set_dashboard_mode(bool enable) {
    s_is_dashboard = enable;
    ESP_LOGI(TAG, "Dashboard mode: %s", enable ? "ON (dashboard.html)" : "OFF (root.html)");
}

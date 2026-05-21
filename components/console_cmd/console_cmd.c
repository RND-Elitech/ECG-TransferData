#include "console_cmd.h"
#include "uploader.h"
#include <dirent.h>
#include <sys/types.h>
#include <string.h>
#include "storage_manager.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_console.h"
#include "sdkconfig.h"

static const char *TAG = "console_cmd";

#define PROMPT_STR CONFIG_IDF_TARGET

static esp_console_repl_t *s_repl = NULL;

/* ─── Handlers ─── */

static int _cmd_upload(int argc, char **argv)
{
    return uploader_run() == ESP_OK ? 0 : -1;
}

static int _cmd_check(int argc, char **argv)
{
    if (storage_manager_in_use_by_usb()) {
        ESP_LOGE(TAG, "Storage sedang dipakai USB Host, tidak bisa diakses");
        return -1;
    }
    uploader_init(NULL); /* Hanya list, tidak upload */

    /* Tampilkan folder ecg_archive */
    DIR *dir = opendir("/data");
    if (!dir) { ESP_LOGE(TAG, "Gagal membuka /data"); return -1; }

    struct dirent *entry;
    bool found = false;
    printf("Folder ecg_archive yang ditemukan:\n");
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && strncmp(entry->d_name, "ecg_archive", 11) == 0) {
            printf("  [DIR] %s\n", entry->d_name);
            found = true;
            char folder_path[512];
            snprintf(folder_path, sizeof(folder_path), "/data/%s", entry->d_name);
            DIR *sub = opendir(folder_path);
            if (sub) {
                struct dirent *se;
                while ((se = readdir(sub)) != NULL) {
                    if (se->d_type == DT_REG &&
                        (strstr(se->d_name, ".xml") || strstr(se->d_name, ".XML") ||
                         strstr(se->d_name, ".jpg") || strstr(se->d_name, ".JPG") ||
                         strstr(se->d_name, ".bmp") || strstr(se->d_name, ".BMP"))) {
                        printf("       - %s\n", se->d_name);
                    }
                }
                closedir(sub);
            }
        }
    }
    closedir(dir);
    if (!found) printf("  (tidak ada folder ecg_archive)\n");
    return 0;
}

static int _cmd_size(int argc, char **argv)
{
    if (storage_manager_in_use_by_usb()) {
        ESP_LOGE(TAG, "Storage sedang dipakai USB Host");
        return -1;
    }
    uint32_t sec_count = storage_manager_get_sector_count();
    uint32_t sec_size  = storage_manager_get_sector_size();
    printf("Kapasitas: %llu MB\n",
           ((uint64_t)sec_count) * sec_size / (1024 * 1024));
    return 0;
}

static int _cmd_mount(int argc, char **argv)
{
    if (!storage_manager_in_use_by_usb()) {
        ESP_LOGE(TAG, "Storage sudah di-mount ke aplikasi");
        return -1;
    }
    return storage_manager_mount() == ESP_OK ? 0 : -1;
}

static int _cmd_expose(int argc, char **argv)
{
    if (storage_manager_in_use_by_usb()) {
        ESP_LOGE(TAG, "Storage sudah di-expose ke USB Host");
        return -1;
    }
    return storage_manager_expose_to_usb() == ESP_OK ? 0 : -1;
}

static int _cmd_status(int argc, char **argv)
{
    printf("Storage di USB Host: %s\n",
           storage_manager_in_use_by_usb() ? "Ya" : "Tidak");
    return 0;
}

static int _cmd_set_sn(int argc, char **argv)
{
    if (argc < 2) {
        printf("Gunakan: set_sn <SERIAL_NUMBER>\n");
        printf("Contoh: set_sn DONGLE-001\n");
        return -1;
    }
    
    const char *sn = argv[1];
    if (strlen(sn) >= 32) {
        printf("Error: Serial Number terlalu panjang (maksimal 31 karakter)\n");
        return -1;
    }

    nvs_handle_t nvs_handle;
    /* Buka namespace "factory" di NVS default */
    esp_err_t err = nvs_open("factory", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        printf("Error membuka NVS (0x%x). Pastikan nvs_flash_init() sudah dipanggil.\n", err);
        return -1;
    }

    err = nvs_set_str(nvs_handle, "dongle_sn", sn);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        printf("SUKSES! Serial Number disetel ke: '%s'\n", sn);
        printf("Silakan cabut pasang USB Dongle (Restart) agar perubahan nama WiFi dan Flashdisk diterapkan.\n");
    } else {
        printf("Gagal menyimpan ke NVS (0x%x)\n", err);
    }
    return 0;
}

/* ─── Daftar command ─── */
static const esp_console_cmd_t s_cmds[] = {
    {
        .command = "upload",
        .help    = "Upload file ECG dari folder ecg_archive ke server",
        .hint    = NULL,
        .func    = &_cmd_upload,
    },
    {
        .command = "check",
        .help    = "Tampilkan daftar folder ecg_archive dan file yang ada",
        .hint    = NULL,
        .func    = &_cmd_check,
    },
    {
        .command = "size",
        .help    = "Tampilkan kapasitas storage",
        .hint    = NULL,
        .func    = &_cmd_size,
    },
    {
        .command = "mount",
        .help    = "Ambil alih storage ke aplikasi",
        .hint    = NULL,
        .func    = &_cmd_mount,
    },
    {
        .command = "expose",
        .help    = "Serahkan storage ke USB Host",
        .hint    = NULL,
        .func    = &_cmd_expose,
    },
    {
        .command = "status",
        .help    = "Status kepemilikan storage",
        .hint    = NULL,
        .func    = &_cmd_status,
    },
    {
        .command = "set_sn",
        .help    = "Suntikkan Serial Number Dongle ke memori permanen (NVS)",
        .hint    = "<SERIAL_NUMBER>",
        .func    = &_cmd_set_sn,
    },
};

/* ─── Public API ─── */

esp_err_t console_cmd_init(void)
{
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.task_stack_size    = 92160;
    repl_cfg.prompt             = PROMPT_STR ">";
    repl_cfg.max_cmdline_length = 64;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_cfg, &repl_cfg, &s_repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) && defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &s_repl));
#else
#error "Tipe console tidak didukung"
#endif

    for (int i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&s_cmds[i]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(s_repl));
    ESP_LOGI(TAG, "Console REPL siap");
    return ESP_OK;
}

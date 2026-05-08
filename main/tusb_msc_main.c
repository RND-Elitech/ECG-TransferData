#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "esp_console.h"
#include "esp_check.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "esp_http_client.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SDMMC
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#endif

/* Warning for secondary serial console */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#if !CONFIG_ESP_CONSOLE_SECONDARY_NONE
#warning "A secondary serial console is not useful when using the console component. Please disable it in menuconfig."
#endif
#endif

static const char *TAG = "tusb_msc_main";
static esp_console_repl_t *repl = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL;

/* TinyUSB descriptors */
#define EPNUM_MSC       1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

enum {
    EDPT_CTRL_OUT = 0x00,
    EDPT_CTRL_IN  = 0x80,
    EDPT_MSC_OUT  = 0x01,
    EDPT_MSC_IN   = 0x81,
};

static tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // Espressif VID
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static uint8_t const msc_fs_configuration_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};

static uint8_t const msc_hs_configuration_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};
#endif

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  // 0: English (0x0409)
    "TinyUSB",                      // 1: Manufacturer
    "TinyUSB Device",               // 2: Product
    "123456",                       // 3: Serials
    "Example MSC",                  // 4: MSC
};

#define BASE_PATH "/data" // Base path to mount the partition
#define PROMPT_STR CONFIG_IDF_TARGET

/* Function prototypes */
static int console_unmount(int argc, char **argv);
static int console_size(int argc, char **argv);
static int console_status(int argc, char **argv);
static int console_check(int argc, char **argv);
static int console_upload(int argc, char **argv);
static int console_mount(int argc, char **argv);
static void storage_mount_changed_cb(tinyusb_msc_event_t *event);
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void _mount(void); // Ditambahkan untuk mengatasi implicit declaration
static void cleanup_initial_storage(void);

/* Command structure */
const esp_console_cmd_t cmds[] = {
    {
        .command = "upload",
        .help = "Unggah file XML dari folder ecg_archive secara otomatis",
        .hint = NULL, // Hint dihapus karena tidak perlu argumen
        .func = &console_upload,
    },
    {
        .command = "check",
        .help = "List folders matching ecg_archive in /data and their XML files",
        .hint = NULL,
        .func = &console_check,
    },
    {
        .command = "size",
        .help = "Show storage size and sector size",
        .hint = NULL,
        .func = &console_size,
    },
    {
        .command = "mount",
        .help = "Mount storage to application",
        .hint = NULL,
        .func = &console_mount,
    },
    {
        .command = "expose",
        .help = "Expose Storage to Host",
        .hint = NULL,
        .func = &console_unmount,
    },
    {
        .command = "status",
        .help = "Status of storage exposure over USB",
        .hint = NULL,
        .func = &console_status,
    }
};

static int console_upload_core(int argc, char **argv)
{
    // Cari folder ecg_archive
    DIR *dir = opendir(BASE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Gagal membuka direktori %s", BASE_PATH);
        return -1;
    }

    struct dirent *entry;
    char latest_folder[256] = "";
    int max_index = -1;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && strncmp(entry->d_name, "ecg_archive", 11) == 0) {
            if (strcmp(entry->d_name, "ecg_archive") == 0) {
                if (max_index < 0) {
                    strcpy(latest_folder, entry->d_name);
                    max_index = 0;
                }
            } else {
                int index = atoi(entry->d_name + 11); // Lewati "ecg_archive_"
                if (index > max_index) {
                    max_index = index;
                    strcpy(latest_folder, entry->d_name);
                }
            }
        }
    }
    closedir(dir);

    if (max_index == -1 && strlen(latest_folder) == 0) {
        ESP_LOGE(TAG, "Tidak ditemukan folder ecg_archive di %s", BASE_PATH);
        return -1;
    }

    // Cari file XML di folder terbaru
    char folder_path[512];
    snprintf(folder_path, sizeof(folder_path), "%s/%s", BASE_PATH, latest_folder);
    DIR *subdir = opendir(folder_path);
    if (!subdir) {
        ESP_LOGE(TAG, "Gagal membuka folder %s", folder_path);
        return -1;
    }

    int success_count = 0;
    int fail_count = 0;
    bool found_xml = false;

    while ((entry = readdir(subdir)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->d_name, ".XML")) {
            found_xml = true;
            char xml_file[256];
            strcpy(xml_file, entry->d_name);

            // Cek file
            char file_path[768];
            snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, xml_file);

            FILE *fp = fopen(file_path, "rb");
            if (!fp) {
                ESP_LOGE(TAG, "Gagal membuka file: %s", file_path);
                fail_count++;
                continue;
            }

            // Dapatkan ukuran file
            fseek(fp, 0, SEEK_END);
            size_t file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            // Validasi ukuran file
            if (file_size < 10) {
                ESP_LOGE(TAG, "File terlalu kecil: %s", file_path);
                fclose(fp);
                fail_count++;
                continue;
            }

            // Boundary untuk multipart
            const char *boundary = "----ESP32FormBoundary123456";

            // Susun header multipart
            char multipart_header[512];
            snprintf(multipart_header, sizeof(multipart_header),
                     "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: application/xml\r\n\r\n",
                     boundary, xml_file);

            // Susun footer multipart
            char multipart_footer[64]; // Diperbesar dari 32 ke 64 untuk menampung string lengkap
            snprintf(multipart_footer, sizeof(multipart_footer), "\r\n--%s--\r\n", boundary);

            // Total panjang konten untuk multipart
            size_t total_content_length = strlen(multipart_header) + file_size + strlen(multipart_footer);

            // Konfigurasi HTTP client
            esp_http_client_config_t config = {
                .url = "http://192.168.13.156:3000/api/ecg-1200g/upload",
                .method = HTTP_METHOD_POST,
                .timeout_ms = 30000,
                .buffer_size = 2048,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (!client) {
                ESP_LOGE(TAG, "Gagal inisialisasi HTTP client");
                fclose(fp);
                fail_count++;
                continue;
            }

            // Set header Content-Type
            char content_type[128];
            snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
            esp_http_client_set_header(client, "Content-Type", content_type);

            // Buka koneksi dengan ukuran diketahui
            esp_err_t err = esp_http_client_open(client, total_content_length);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Gagal membuka HTTP: %s", esp_err_to_name(err));
                esp_http_client_cleanup(client);
                fclose(fp);
                fail_count++;
                continue;
            }

            ESP_LOGI(TAG, "Mengunggah file: %s (%d bytes) dari folder: %s", xml_file, file_size, latest_folder);

            // Kirim header multipart
            if (esp_http_client_write(client, multipart_header, strlen(multipart_header)) < 0) {
                ESP_LOGE(TAG, "Gagal menulis header multipart");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fclose(fp);
                fail_count++;
                continue;
            }

            // Kirim isi file
            uint8_t buffer[1024];
            size_t total_read = 0;
            bool write_failed = false;
            while (total_read < file_size) {
                size_t to_read = file_size - total_read < sizeof(buffer) ? file_size - total_read : sizeof(buffer);
                size_t read_bytes = fread(buffer, 1, to_read, fp);
                if (read_bytes <= 0) {
                    ESP_LOGE(TAG, "Gagal membaca file");
                    write_failed = true;
                    break;
                }
                if (esp_http_client_write(client, (char *)buffer, read_bytes) < 0) {
                    ESP_LOGE(TAG, "Gagal menulis data file");
                    write_failed = true;
                    break;
                }
                total_read += read_bytes;
            }

            if (write_failed) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fclose(fp);
                fail_count++;
                continue;
            }

            // Kirim footer multipart
            if (esp_http_client_write(client, multipart_footer, strlen(multipart_footer)) < 0) {
                ESP_LOGE(TAG, "Gagal menulis footer multipart");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fclose(fp);
                fail_count++;
                continue;
            }

            // Dapatkan respons
            int status = esp_http_client_fetch_headers(client);
            if (status < 0) {
                ESP_LOGE(TAG, "Gagal mengambil header respons");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fclose(fp);
                fail_count++;
                continue;
            }

            int response_content_length = esp_http_client_get_content_length(client);
            int status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "Status HTTP: %d, Content-Length: %d", status_code, response_content_length);

            if (response_content_length > 0) {
                char *response_buffer = malloc(response_content_length + 1);
                if (response_buffer) {
                    int read_len = esp_http_client_read_response(client, response_buffer, response_content_length);
                    if (read_len > 0) {
                        response_buffer[read_len] = '\0';
                        ESP_LOGI(TAG, "Respons dari server: %s", response_buffer);
                    } else {
                        ESP_LOGE(TAG, "Gagal membaca respons dari server, read_len: %d", read_len);
                    }
                    free(response_buffer);
                } else {
                    ESP_LOGE(TAG, "Gagal alokasi memori untuk response_buffer");
                }
            } else {
                ESP_LOGI(TAG, "Tidak ada body respons dari server (Content-Length: 0)");
            }

            if (status_code == 200 || status_code == 201) {
                ESP_LOGI(TAG, "Upload berhasil! Menghapus file: %s", file_path);
                unlink(file_path);
                success_count++;
            } else {
                ESP_LOGE(TAG, "Upload gagal dengan status %d", status_code);
                fail_count++;
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fclose(fp);
        }
    }
    closedir(subdir);

    if (!found_xml) {
        ESP_LOGE(TAG, "Tidak ditemukan file XML di %s", folder_path);
        return -1;
    }

    if (success_count > 0 && fail_count == 0) {
        ESP_LOGI(TAG, "Semua file (%d) berhasil diupload, menghapus folder %s...", success_count, folder_path);
        rmdir(folder_path);
    } else {
        ESP_LOGW(TAG, "Selesai. Sukses: %d, Gagal: %d", success_count, fail_count);
    }

    return (fail_count == 0) ? 0 : -1;
}

static int console_upload(int argc, char **argv)
{
    bool needs_unmount = false;

    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGI(TAG, "Storage dipakai USB. Mengambil alih untuk upload...");
        if (tinyusb_msc_storage_mount(BASE_PATH) != ESP_OK) {
            ESP_LOGE(TAG, "Gagal mengambil alih storage.");
            return -1;
        }
        needs_unmount = true;
    }

    int ret = console_upload_core(argc, argv);

    if (needs_unmount) {
        ESP_LOGI(TAG, "Upload selesai. Mengembalikan storage ke USB Host...");
        tinyusb_msc_storage_unmount();
    }

    return ret;
}

static int console_check(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Storage exposed over USB. Application can't access storage.");
        return -1;
    }

    ESP_LOGI(TAG, "Listing folders in %s", BASE_PATH);
    DIR *dir = opendir(BASE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s", BASE_PATH);
        return -1;
    }

    bool found = false;
    struct dirent *entry;
    printf("Folders found:\n");
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && strncmp(entry->d_name, "ecg_archive", 11) == 0) {
            printf("- %s\n", entry->d_name);
            found = true;

            // List XML files in the folder
            char folder_path[512];
            snprintf(folder_path, sizeof(folder_path), "%s/%s", BASE_PATH, entry->d_name);
            DIR *subdir = opendir(folder_path);
            if (subdir) {
                printf("  XML files:\n");
                struct dirent *subentry;
                bool file_found = false;
                while ((subentry = readdir(subdir)) != NULL) {
                    if (subentry->d_type == DT_REG && strstr(subentry->d_name, ".XML")) {
                        printf("    - %s\n", subentry->d_name);
                        file_found = true;
                    }
                }
                if (!file_found) {
                    printf("    (No XML files found)\n");
                }
                closedir(subdir);
            } else {
                ESP_LOGE(TAG, "Failed to open directory %s", folder_path);
            }
        }
    }
    closedir(dir);

    if (!found) {
        ESP_LOGI(TAG, "No ecg_archive folders found in %s", BASE_PATH);
    }
    return 0;
}

static int console_size(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Storage exposed over USB. Application can't access storage");
        return -1;
    }
    uint32_t sec_count = tinyusb_msc_storage_get_sector_count();
    uint32_t sec_size = tinyusb_msc_storage_get_sector_size();
    printf("Storage Capacity %lluMB\n", ((uint64_t) sec_count) * sec_size / (1024 * 1024));
    return 0;
}

static int console_unmount(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Storage is already exposed");
        return -1;
    }
    ESP_LOGI(TAG, "Unmount storage...");
    ESP_ERROR_CHECK(tinyusb_msc_storage_unmount());
    return 0;
}

static int console_status(int argc, char **argv)
{
    printf("Storage exposed over USB: %s\n", tinyusb_msc_storage_in_use_by_usb_host() ? "Yes" : "No");
    return 0;
}

static int console_mount(int argc, char **argv)
{
    if (!tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Storage is already mounted to application");
        return -1;
    }
    ESP_LOGI(TAG, "Mounting storage to application...");
    _mount();
    return 0;
}

static void background_upload_task(void *pvParameters) {
    ESP_LOGI(TAG, "Menjalankan proses upload dari background task...");
    console_upload(0, NULL);
    vTaskDelete(NULL);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Berhasil terhubung ke broker MQTT (mqtts://dev.samelement.com:8883)");
            esp_mqtt_client_subscribe(mqtt_client, "ecg1200G/upload", 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Terputus dari MQTT broker");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Berhasil subscribe ke topik: ecg1200G/upload");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Menerima pesan di topik %.*s: %.*s", event->topic_len, event->topic, event->data_len, event->data);
            if (event->topic_len == strlen("ecg1200G/upload") && strncmp(event->topic, "ecg1200G/upload", event->topic_len) == 0) {
                // Trigger upload
                if (event->data_len == strlen("upload") && strncmp(event->data, "upload", event->data_len) == 0) {
                    ESP_LOGI(TAG, "Menerima perintah upload, memulai proses di background...");
                    xTaskCreate(&background_upload_task, "bg_upload", 8192, NULL, 5, NULL);
                } else {
                    ESP_LOGW(TAG, "Payload tidak valid, abaikan. Harus 'upload'");
                }
            }
            break;
        default:
            ESP_LOGI(TAG, "Event MQTT lainnya: %d", event->event_id);
            break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = "mqtts://dev.samelement.com",
            .address.port = 8888,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .username = "iotgateway",
            .authentication.password = "iotgateway10nice",
        },
        .task = {
            .stack_size = 10240, // Tingkatkan ukuran stack ke 10KB
            .priority = 5,       // Prioritas default
        },
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    /* Register event handler */
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    ESP_LOGI(TAG, "Storage mounted to application: %s", event->mount_changed_data.is_mounted ? "Yes" : "No");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi terputus, mencoba menghubungkan kembali...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi terhubung, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void cleanup_initial_storage(void)
{
    ESP_LOGI(TAG, "Membersihkan direktori penyimpanan awal...");
    DIR *dir = opendir(BASE_PATH);
    if (dir != NULL) {
        struct dirent *entry;
        char full_path[512];
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                snprintf(full_path, sizeof(full_path), "%s/%s", BASE_PATH, entry->d_name);
                if (entry->d_type == DT_DIR) {
                    DIR *subdir = opendir(full_path);
                    if (subdir != NULL) {
                        struct dirent *subentry;
                        char sub_path[768];
                        while ((subentry = readdir(subdir)) != NULL) {
                            if (strcmp(subentry->d_name, ".") != 0 && strcmp(subentry->d_name, "..") != 0) {
                                snprintf(sub_path, sizeof(sub_path), "%s/%s", full_path, subentry->d_name);
                                ESP_LOGI(TAG, "Menghapus file lama: %s", sub_path);
                                unlink(sub_path);
                            }
                        }
                        closedir(subdir);
                    }
                    ESP_LOGI(TAG, "Menghapus folder lama: %s", full_path);
                    rmdir(full_path);
                } else {
                    ESP_LOGI(TAG, "Menghapus file lama: %s", full_path);
                    unlink(full_path);
                }
            }
        }
        closedir(dir);
        ESP_LOGI(TAG, "Pembersihan awal selesai.");
    } else {
        ESP_LOGW(TAG, "Gagal membuka direktori untuk pembersihan.");
    }
}

static void _mount(void)
{
    ESP_LOGI(TAG, "Mount storage...");
    ESP_ERROR_CHECK(tinyusb_msc_storage_mount(BASE_PATH));

    ESP_LOGI(TAG, "\nls command output:");
    struct dirent *d;
    DIR *dh = opendir(BASE_PATH);
    if (!dh) {
        if (errno == ENOENT) {
            ESP_LOGE(TAG, "Directory doesn't exist %s", BASE_PATH);
        } else {
            ESP_LOGE(TAG, "Unable to read directory %s", BASE_PATH);
        }
        return;
    }
    while ((d = readdir(dh)) != NULL) {
        printf("%s\n", d->d_name);
    }
    closedir(dh);
}

#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    ESP_LOGI(TAG, "Initializing wear levelling");
    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition. Check the partition table.");
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(data_partition, wl_handle);
}
#else
static esp_err_t storage_init_sdmmc(sdmmc_card_t **card)
{
    esp_err_t ret = ESP_OK;
    bool host_init = false;
    sdmmc_card_t *sd_card;

    ESP_LOGI(TAG, "Initializing SDCard");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create a new on-chip LDO power control driver");
        return ret;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
    slot_config.width = 4;
#else
    slot_config.width = 1;
#endif
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = CONFIG_EXAMPLE_PIN_CLK;
    slot_config.cmd = CONFIG_EXAMPLE_PIN_CMD;
    slot_config.d0 = CONFIG_EXAMPLE_PIN_D0;
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
    slot_config.d1 = CONFIG_EXAMPLE_PIN_D1;
    slot_config.d2 = CONFIG_EXAMPLE_PIN_D2;
    slot_config.d3 = CONFIG_EXAMPLE_PIN_D3;
#endif
#endif
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sd_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    ESP_GOTO_ON_FALSE(sd_card, ESP_ERR_NO_MEM, clean, TAG, "Could not allocate new sdmmc_card_t");

    ESP_GOTO_ON_ERROR((*host.init)(), clean, TAG, "Host Config Init fail");
    host_init = true;

    ESP_GOTO_ON_ERROR(sdmmc_host_init_slot(host.slot, (const sdmmc_slot_config_t *) &slot_config),
                      clean, TAG, "Host init slot fail");

    while (sdmmc_card_init(&host, sd_card)) {
        ESP_LOGE(TAG, "The detection pin of the slot is disconnected(Insert uSD card). Retrying...");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    sdmmc_card_print_info(stdout, sd_card);
    *card = sd_card;
    return ESP_OK;

clean:
    if (host_init) {
        if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
            host.deinit_p(host.slot);
        } else {
            (*host.deinit)();
        }
    }
    if (sd_card) {
        free(sd_card);
    }
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle);
#endif
    return ret;
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "Menginisialisasi Wi-Fi...");

    // Inisialisasi NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inisialisasi TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Inisialisasi Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Daftarkan handler untuk event Wi-Fi
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Konfigurasi Wi-Fi sebagai station
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Elitech",
            .password = "wifis1nko",
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Definisi IP statis (hardcode, bisa dikomentari untuk menggunakan DHCP)
    #define USE_STATIC_IP
    #ifdef USE_STATIC_IP
    // esp_netif_ip_info_t ip_info = {
    //     .ip = {
    //         .addr = ipaddr_addr("192.168.13.100") // Ganti dengan IP statis yang diinginkan
    //     },
    //     .netmask = {
    //         .addr = ipaddr_addr("255.255.255.0") // Netmask
    //     },
    //     .gw = {
    //         .addr = ipaddr_addr("192.168.13.1")  // Gateway
    //     },
    // };
    // esp_netif_dhcpc_stop(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")); // Matikan DHCP
    // esp_netif_set_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
    // ESP_LOGI(TAG, "Menggunakan IP statis: " IPSTR ", Netmask: " IPSTR ", Gateway: " IPSTR,
    //          IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
    #else
    esp_netif_dhcpc_start(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")); // Aktifkan DHCP
    ESP_LOGI(TAG, "Menggunakan IP dinamis (DHCP)");
    #endif

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Menunggu koneksi Wi-Fi...");
    // Tunggu hingga terhubung (timeout 30 detik)
    TickType_t start_time = xTaskGetTickCount();
    while (true) {
        esp_netif_ip_info_t ip_info_check;
        if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info_check) == ESP_OK &&
            ip_info_check.ip.addr != 0) {
            #ifdef USE_STATIC_IP
            ESP_LOGI(TAG, "Wi-Fi berhasil terhubung, IP: " IPSTR, IP2STR(&ip_info_check.ip));
            #else
            ESP_LOGI(TAG, "Wi-Fi berhasil terhubung, IP: " IPSTR ", Netmask: " IPSTR ", Gateway: " IPSTR,
                     IP2STR(&ip_info_check.ip), IP2STR(&ip_info_check.netmask), IP2STR(&ip_info_check.gw));
            #endif
            mqtt_app_start(); // Mulai MQTT setelah Wi-Fi terhubung
            break;
        }
        if ((xTaskGetTickCount() - start_time) > pdMS_TO_TICKS(30000)) {
            ESP_LOGE(TAG, "Gagal terhubung ke Wi-Fi dalam 30 detik");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Ping ke server 192.168.13.145...");
    system("ping 192.168.13.145");

    ESP_LOGI(TAG, "Initializing storage...");
#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));
    const tinyusb_msc_spiflash_config_t config_spi = {
        .wl_handle = wl_handle,
        .callback_mount_changed = storage_mount_changed_cb,
        .mount_config.max_files = 5,
    };
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&config_spi));
#else
    static sdmmc_card_t *card = NULL;
    ESP_ERROR_CHECK(storage_init_sdmmc(&card));
    const tinyusb_msc_sdmmc_config_t config_sdmmc = {
        .card = card,
        .callback_mount_changed = storage_mount_changed_cb,
        .mount_config.max_files = 5,
    };
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_sdmmc(&config_sdmmc));
#endif
    ESP_ERROR_CHECK(tinyusb_msc_register_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED, storage_mount_changed_cb));

    // Mount storage (akan di-unmount kembali di bawah)
    _mount();

    // Pembersihan direktori saat baru dicolokkan (awal)
    cleanup_initial_storage();

    // Pindahkan unmount setelah inisialisasi TinyUSB
    ESP_LOGI(TAG, "Mengatur mode default expose...");
    ESP_ERROR_CHECK(tinyusb_msc_storage_unmount());

    ESP_LOGI(TAG, "USB MSC initialization");
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &descriptor_config,
        .string_descriptor = string_desc_arr,
        .string_descriptor_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]),
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = msc_fs_configuration_desc,
        .hs_configuration_descriptor = msc_hs_configuration_desc,
        .qualifier_descriptor = &device_qualifier,
#else
        .configuration_descriptor = msc_fs_configuration_desc,
#endif
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB MSC initialization DONE");

    // Initialize console
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.task_stack_size = 92160;
    repl_config.prompt = PROMPT_STR ">";
    repl_config.max_cmdline_length = 64;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) && defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
#error Unsupported console type
#endif

    for (int count = 0; count < sizeof(cmds) / sizeof(esp_console_cmd_t); count++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[count]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
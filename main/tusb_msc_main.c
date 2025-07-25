/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* DESCRIPTION:
 * This example makes an ESP32-based device recognizable as a USB Mass Storage Device.
 * It reads an XML file from the storage (SPI Flash or SD card), parses patient information,
 * and displays it on the serial console. The storage can be accessed by either the embedded
 * application or the USB host, but not both simultaneously.
 */

#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "sdkconfig.h"
#include "esp_console.h"
#include "esp_check.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SDMMC
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
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

static const char *TAG = "example_main";
static esp_console_repl_t *repl = NULL;

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

/* Patient information structure */
typedef struct {
    char name[64];
    char sex[16];
    char age[16];
    char sickroomid[16];
    char bedid[16];
    char inhospitalid[32];
    char cop[32];
    char hr[16];              // Heart Rate
    char birth_time[32];
    char effective_time_low[32];
    char effective_time_high[32];
    char caseid[32];
    char filter[16];
    char uniquecode[64];
    char pr_interval[16];     // PR Interval
    char p_duration[16];      // P Duration
    char qrs_duration[16];    // QRS Duration
    char t_duration[16];      // T Duration (new)
    char qt_interval[16];     // QT Interval
    char qtc_interval[16];    // QTc Interval
    char p_axis[16];          // P Axis
    char qrs_axis[16];        // QRS Axis
    char t_axis[16];          // T Axis
    char r_v5[16];            // R in V5
    char s_v1[16];            // S in V1
    char interpretation[256]; // ECG Interpretation Statement
    char lead_i[4096];        // Data for MDC_ECG_LEAD_I
    char lead_ii[4096];       // Data for MDC_ECG_LEAD_II
    char lead_iii[4096];      // Data for MDC_ECG_LEAD_III
    char lead_avr[4096];      // Data for MDC_ECG_LEAD_AVR
    char lead_avl[4096];      // Data for MDC_ECG_LEAD_AVL
    char lead_avf[4096];      // Data for MDC_ECG_LEAD_AVF
    char lead_v1[4096];       // Data for MDC_ECG_LEAD_V1
    char lead_v2[4096];       // Data for MDC_ECG_LEAD_V2
    char lead_v3[4096];       // Data for MDC_ECG_LEAD_V3
    char lead_v4[4096];       // Data for MDC_ECG_LEAD_V4
    char lead_v5[4096];       // Data for MDC_ECG_LEAD_V5
    char lead_v6[4096];       // Data for MDC_ECG_LEAD_V6
    char origin[16];          // Origin value and unit
    char scale[16];           // Scale value and unit
    char increment[16];       // Increment value and unit
} patient_info_t;

/* Function prototypes */
static int console_unmount(int argc, char **argv);
static int console_read(int argc, char **argv);
static int console_write(int argc, char **argv);
static int console_size(int argc, char **argv);
static int console_status(int argc, char **argv);
static int console_exit(int argc, char **argv);
static void parse_xml_file(const char *filename, patient_info_t *info);
static int console_check(int argc, char **argv);
static int console_read_file(int argc, char **argv);

const esp_console_cmd_t cmds[] = {
    {
        .command = "check",
        .help = "List folders matching ecg_archive in /data and their XML files",
        .hint = NULL,
        .func = &console_check,
    },
    {
        .command = "read_file",
        .help = "Read and parse XML file from specified folder (e.g., read_file ecg_archive data.XML)",
        .hint = "<folder_name> <file_name>",
        .func = &console_read_file,
    },
    {
        .command = "read",
        .help = "Read and parse XML file in latest ecg_archive folder",
        .hint = NULL,
        .func = &console_read,
    },
    {
        .command = "write",
        .help = "Create file BASE_PATH/README.MD if it does not exist",
        .hint = NULL,
        .func = &console_write,
    },
    {
        .command = "size",
        .help = "Show storage size and sector size",
        .hint = NULL,
        .func = &console_size,
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
    },
    {
        .command = "exit",
        .help = "Exit from application",
        .hint = NULL,
        .func = &console_exit,
    }
};

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
            char folder_path[512]; // Increased buffer size to avoid truncation
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

static int console_read_file(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Storage exposed over USB. Application can't read from storage.");
        return -1;
    }

    if (argc != 3) {
        ESP_LOGE(TAG, "Usage: read_file <folder_name> <file_name>");
        return -1;
    }

    const char *folder_name = argv[1];
    const char *file_name = argv[2];

    // Validate folder name
    if (strncmp(folder_name, "ecg_archive", 11) != 0) {
        ESP_LOGE(TAG, "Folder must start with 'ecg_archive'");
        return -1;
    }

    // Check if folder exists
    char folder_path[512];
    snprintf(folder_path, sizeof(folder_path), "%s/%s%", BASE_PATH, folder_name);
    DIR *dir = opendir(folder_path);
    if (!dir) {
        ESP_LOGE(TAG, "Folder %s does not exist", folder_path);
        return -1;
    }
    closedir(dir);

    // Check if file exists and has .XML extension
    char file_path[768];
    snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, file_name);
    if (!strstr(file_name, ".XML")) {
        ESP_LOGE(TAG, "File must have .XML extension");
        return -1;
    }

    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file %s", file_path);
        return -1;
    }
    fclose(fp);

    // Parse the XML file
    ESP_LOGI(TAG, "Reading XML file: %s/%s%", folder_name, file_name);
    patient_info_t info;
    parse_xml_file(file_path, &info);

    // Print patient information
    printf("\n=== Informasi Pasien ===\n");
    printf("Nama Pasien     : %s\n", info.name);
    printf("Jenis Kelamin   : %s\n", info.sex);
    printf("Umur            : %s tahun\n", info.age);
    printf("Tanggal Lahir   : %s\n", info.birth_time);
    printf("Room ID         : %s\n", info.sickroomid);
    printf("Bed ID         : %s\n", info.bedid);
    printf("Inhospital ID        : %s\n", info.inhospital_id);
    printf("Operator        : %s\n", info.cop);
    printf("Waktu Awal      : %s\n", info.effective_time_low);
    printf("Waktu Akhir     : %s\n", info.effective_time_high);
    printf("Case ID         : %s\n", info.casesize);
    printf("Filter          : %s\n", info.filter);
    printf("Kode Unik       : %s\n", info.uniquecode);
    printf("=======================\n");

    // Print EKG interpretation
    printf("\n=== Hasil Interpretasi EKG\n");
    printf("Heart Rate jing      : %s\n", info.hr);
    printf("Interval PR     : %s\n", info.pr_intervals);
    printf("Durasi P woi       : %s\n", info.p_duration);
    printf("Durasi QRS      : %s\n", info.qrs_duration);
    printf("Durasi T        : %s\n", info.t_duration);
    printf("Durasi QT       : %s\n", info.qt_intervals);
    printf("QT Koreksi      : %s\n", info.qtc_intervals);
    printf("Sudut Axis P    : %s\n", info.p_axis);
    printf("Sudut Axis QRS  : %s\n", info.qrs_axis);
    printf("Sudut Axis T    : %s\n", info.t_axis);
    printf("Amplitudo R V5  : %s\n", info.r_v);
    printf("Amplitudo S V1  : %s\n", info.s_v);
    printf("Interpretasi    : %s\n", info.interaction);
    printf("=== Data Grafik EKG ===\n");
    printf("Lead I          : %s\n", info.leadi_i);
    printf("Lead II         : %s\n", info.leadi_ii_lead_ii);
    printf("Lead III        : %s\n", info.leadi_iii_lead_);
    printf("Lead aVR      : %s\n", info.leadi_avr_lead_);
    printf("Lead aVL : %s\n", info.leadi_avl_);
    printf("Lead aVF : %s\n", info.leadi_avf_);
    printf("Lead V1         : %s\n", info.leadi_v1_lead_);
    printf("Lead V2         : %s\n", info.leadi_v2_lead_);
    printf("Lead V3         : %s\n", info.leadi_v3_lead_);
    printf("Lead V4        : %s\n", info.leadi_v4_lead_);
    printf("Lead V5         : %s\n", info.leadi_v5_lead_);
    printf("Lead V6        : %s\n", info.leadi_v6_lead_);
    printf("Origin         : %s\n", info.origin_);
    printf("Scale           : %s\n", info.scale_);
    printf("Increment       : %s\n", info.increment_);
    printf("==============================\n");

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

static int console_write(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Storage exposed over USB. Application can't write to storage.");
        return -1;
    }
    ESP_LOGD(TAG, "Write to storage:");
    const char *filename = BASE_PATH "/README.MD";
    FILE *fd = fopen(filename, "r");
    if (!fd) {
        ESP_LOGW(TAG, "README.MD doesn't exist yet, creating");
        fd = fopen(filename, "w");
        fprintf(fd, "Mass Storage Devices are one of the most common USB devices.\n");
        fprintf(fd, "In this example, ESP chip will be recognized as a Mass Storage Device.\n");
        fclose(fd);
    } else {
        fclose(fd);
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

static int console_status(int argc, char **argv)
{
    printf("Storage exposed over USB: %s\n", tinyusb_msc_storage_in_use_by_usb_host() ? "Yes" : "No");
    return 0;
}

static int console_exit(int argc, char **argv)
{
    tinyusb_msc_unregister_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED);
    tinyusb_msc_storage_deinit();
    tinyusb_driver_uninstall();
    printf("Application Exit\n");
    repl->del(repl);
    return 0;
}

/* Simple XML parser for patient information */
sstatic void parse_xml_file(const char *path, patient_info_t *info)
{
    // Initialize patient info with default values
    strcpy(info->name, "N/A");
    strcpy(info->sex, "N/A");
    strcpy(info->age, "N/A");
    strcpy(info->sickroomid, "N/A");
    strcpy(info->bedid, "N/A");
    strcpy(info->inhospitalid, "N/A");
    strcpy(info->cop, "N/A");
    strcpy(info->hr, "N/A");
    strcpy(info->birth_time, "N/A");
    strcpy(info->effective_time_low, "N/A");
    strcpy(info->effective_time_high, "N/A");
    strcpy(info->caseid, "N/A");
    strcpy(info->filter, "N/A");
    strcpy(info->uniquecode, "N/A");
    strcpy(info->pr_interval, "N/A");
    strcpy(info->p_duration, "N/A");
    strcpy(info->qrs_duration, "N/A");
    strcpy(info->t_duration, "N/A");
    strcpy(info->qt_interval, "N/A");
    strcpy(info->qtc_interval, "N/A");
    strcpy(info->p_axis, "N/A");
    strcpy(info->qrs_axis, "N/A");
    strcpy(info->t_axis, "N/A");
    strcpy(info->r_v5, "N/A");
    strcpy(info->s_v1, "N/A");
    strcpy(info->interpretation, "N/A");
    strcpy(info->lead_i, "N/A");
    strcpy(info->lead_ii, "N/A");
    strcpy(info->lead_iii, "N/A");
    strcpy(info->lead_avr, "N/A");
    strcpy(info->lead_avl, "N/A");
    strcpy(info->lead_avf, "N/A");
    strcpy(info->lead_v1, "N/A");
    strcpy(info->lead_v2, "N/A");
    strcpy(info->lead_v3, "N/A");
    strcpy(info->lead_v4, "N/A");
    strcpy(info->lead_v5, "N/A");
    strcpy(info->lead_v6, "N/A");
    strcpy(info->origin, "N/A");
    strcpy(info->scale, "N/A");
    strcpy(info->increment, "N/A");

    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file %s", path);
        return;
    }

    char line[512];
    bool in_effective_time = false;
    bool in_annotation = false;
    bool in_slist_pq = false;
    char current_lead[32] = {0};

    while (fgets(line, sizeof(line), fp)) {
        // Remove leading/trailing whitespace
        char *pos = line;
        while (*pos && isspace((unsigned char)*pos)) pos++;
        char *end = pos + strlen(pos);
        while (end > pos && isspace((unsigned char)*(end - 1))) *(--end) = '\0';

        // Check for <effectiveTime> context
        if (strstr(pos, "<effectiveTime>")) {
            in_effective_time = true;
        } else if (strstr(pos, "</effectiveTime>")) {
            in_effective_time = false;
        }

        // Check for <annotation> context
        if (strstr(pos, "<annotation>")) {
            in_annotation = true;
        } else if (strstr(pos, "</annotation>")) {
            in_annotation = false;
        }

        // Check for <value xsi:type="SLIST_PQ"> context
        if (strstr(pos, "<value xsi:type=\"SLIST_PQ\">")) {
            in_slist_pq = true;
        } else if (strstr(pos, "</value>")) {
            in_slist_pq = false;
        }

        // Parse specific tags
        if (strstr(pos, "<subjectDemographicPerson>")) {
            if (fgets(line, sizeof(line), fp)) {
                pos = line;
                while (*pos && isspace((unsigned char)*pos)) pos++;
                end = pos + strlen(pos);
                while (end > pos && isspace((unsigned char)*(end - 1))) *(--end) = '\0';
                if (strncmp(pos, "<name>", 6) == 0) {
                    char *start = pos + 6;
                    char *end_tag = strstr(start, "</name>");
                    if (end_tag) {
                        *end_tag = '\0';
                        strncpy(info->name, start, sizeof(info->name) - 1);
                        info->name[sizeof(info->name) - 1] = '\0';
                    }
                }
            }
        } else if (strstr(pos, "<administrativeGenderCode")) {
            char *code = strstr(pos, "code=\"");
            if (code) {
                code += 6;
                char *end_code = strchr(code, '"');
                if (end_code) {
                    *end_code = '\0';
                    if (strcmp(code, "M") == 0) {
                        strcpy(info->sex, "Pria");
                    } else if (strcmp(code, "F") == 0) {
                        strcpy(info->sex, "Wanita");
                    } else {
                        strcpy(info->sex, "Tidak Diketahui");
                    }
                }
            }
        } else if (strstr(pos, "<Age")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    *end_value = '\0';
                    strncpy(info->age, value, sizeof(info->age) - 1);
                    info->age[sizeof(info->age) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<Room")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    *end_value = '\0';
                    strncpy(info->sickroomid, value, sizeof(info->sickroomid) - 1);
                    info->sickroomid[sizeof(info->sickroomid) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<Bed")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    *end_value = '\0';
                    strncpy(info->bedid, value, sizeof(info->bedid) - 1);
                    info->bedid[sizeof(info->bedid) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<inhospitalid>")) {
            char *start = strstr(pos, "<inhospitalid>");
            if (start) {
                start += 13;
                while (*start && !isalnum((unsigned char)*start)) start++;
                char *end_tag = strstr(start, "</inhospitalid>");
                if (end_tag) {
                    *end_tag = '\0';
                    strncpy(info->inhospitalid, start, sizeof(info->inhospitalid) - 1);
                    info->inhospitalid[sizeof(info->inhospitalid) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<cop>")) {
            char *start = strstr(pos, "<cop>");
            if (start) {
                start += 5;
                char *end_tag = strstr(start, "</cop>");
                if (end_tag) {
                    *end_tag = '\0';
                    strncpy(info->cop, start, sizeof(info->cop) - 1);
                    info->cop[sizeof(info->cop) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<HR")) {
            char *start = strstr(pos, "<HR");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</HR>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        strncpy(info->hr, start, sizeof(info->hr) - 1);
                        info->hr[sizeof(info->hr) - 1] = '\0';
                    }
                }
            }
        } else if (strstr(pos, "<birthTime")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    *end_value = '\0';
                    strncpy(info->birth_time, value, sizeof(info->birth_time) - 1);
                    info->birth_time[sizeof(info->birth_time) - 1] = '\0';
                }
            }
        } else if (in_effective_time && strstr(pos, "<low")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    *end_value = '\0';
                    strncpy(info->effective_time_low, value, sizeof(info->effective_time_low) - 1);
                    info->effective_time_low[sizeof(info->effective_time_low) - 1] = '\0';
                }
            }
        } else if (in_effective_time && strstr(pos, "<high")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    *end_value = '\0';
                    strncpy(info->effective_time_high, value, sizeof(info->effective_time_high) - 1);
                    info->effective_time_high[sizeof(info->effective_time_high) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<caseid>")) {
            char *start = strstr(pos, "<caseid>");
            if (start) {
                start += 8;
                char *end_tag = strstr(start, "</caseid>");
                if (end_tag) {
                    *end_tag = '\0';
                    strncpy(info->caseid, start, sizeof(info->caseid) - 1);
                    info->caseid[sizeof(info->caseid) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<filter>")) {
            char *start = strstr(pos, "<filter>");
            if (start) {
                start += 8;
                char *end_tag = strstr(start, "</filter>");
                if (end_tag) {
                    *end_tag = '\0';
                    strncpy(info->filter, start, sizeof(info->filter) - 1);
                    info->filter[sizeof(info->filter) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<uniquecode>")) {
            char *start = strstr(pos, "<uniquecode>");
            if (start) {
                start += 11;
                while (*start && !isalnum((unsigned char)*start)) start++;
                char *end_tag = strstr(start, "</uniquecode>");
                if (end_tag) {
                    *end_tag = '\0';
                    strncpy(info->uniquecode, start, sizeof(info->uniquecode) - 1);
                    info->uniquecode[sizeof(info->uniquecode) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<PRInterval")) {
            char *start = strstr(pos, "<PRInterval");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</PRInterval>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->pr_interval, sizeof(info->pr_interval), "%s ms", start);
                    }
                }
            }
        } else if (strstr(pos, "<PDuration")) {
            char *start = strstr(pos, "<PDuration");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</PDuration>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->p_duration, sizeof(info->p_duration), "%s ms", start);
                    }
                }
            }
        } else if (strstr(pos, "<QRSDuration")) {
            char *start = strstr(pos, "<QRSDuration");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</QRSDuration>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->qrs_duration, sizeof(info->qrs_duration), "%s ms", start);
                    }
                }
            }
        } else if (strstr(pos, "<TDuration")) {
            char *start = strstr(pos, "<TDuration");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</TDuration>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->t_duration, sizeof(info->t_duration), "%s ms", start);
                    }
                }
            }
        } else if (strstr(pos, "<QTInterval")) {
            char *start = strstr(pos, "<QTInterval");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</QTInterval>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->qt_interval, sizeof(info->qt_interval), "%s ms", start);
                    }
                }
            }
        } else if (strstr(pos, "<QTcInterval")) {
            char *start = strstr(pos, "<QTcInterval");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</QTcInterval>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->qtc_interval, sizeof(info->qtc_interval), "%s ms", start);
                    }
                }
            }
        } else if (strstr(pos, "<PAxis")) {
            char *start = strstr(pos, "<PAxis");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</PAxis>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->p_axis, sizeof(info->p_axis), "%s deg", start);
                    }
                }
            }
        } else if (strstr(pos, "<QRSAxis")) {
            char *start = strstr(pos, "<QRSAxis");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</QRSAxis>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned ևvalue_end)) *value_end-- = '\0';
                        snprintf(info->qrs_axis, sizeof(info->qrs_axis), "%s deg", start);
                    }
                }
            }
        } else if (strstr(pos, "<TAxis")) {
            char *start = strstr(pos, "<TAxis");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</TAxis>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->t_axis, sizeof(info->t_axis), "%s deg", start);
                    }
                }
            }
        } else if (strstr(pos, "<R_V5")) {
            char *start = strstr(pos, "<R_V5");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</R_V5>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->r_v5, sizeof(info->r_v5), "%s mV", start);
                    }
                }
            }
        } else if (strstr(pos, "<S_V1")) {
            char *start = strstr(pos, "<S_V1");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    char *end_tag = strstr(start, "</S_V1>");
                    if (end_tag) {
                        *end_tag = '\0';
                        while (*start && isspace((unsigned char)*start)) start++;
                        char *value_end = end_tag - 1;
                        while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                        snprintf(info->s_v1, sizeof(info->s_v1), "%s mV", start);
                    }
                }
            }
        } else if (in_annotation && strstr(pos, "<value xsi:type=\"ST\">")) {
            char *start = strstr(pos, "<value xsi:type=\"ST\">");
            if (start) {
                start += 20;
                while (*start && !isalnum((unsigned char)*start)) start++;
                char *end_tag = strstr(start, "</value>");
                if (end_tag) {
                    *end_tag = '\0';
                    while (*start && isspace((unsigned char)*start)) start++;
                    char *value_end = end_tag - 1;
                    while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                    strncpy(info->interpretation, start, sizeof(info->interpretation) - 1);
                    info->interpretation[sizeof(info->interpretation) - 1] = '\0';
                }
            }
        } else if (strstr(pos, "<code code=\"MDC_ECG_LEAD_")) {
            char *code = strstr(pos, "code=\"");
            if (code) {
                code += 6;
                char *end_code = strchr(code, '"');
                if (end_code) {
                    *end_code = '\0';
                    strncpy(current_lead, code, sizeof(current_lead) - 1);
                    current_lead[sizeof(current_lead) - 1] = '\0';
                }
            }
        } else if (in_slist_pq && strstr(pos, "<origin")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    char temp[16];
                    *end_value = '\0';
                    strncpy(temp, value, sizeof(temp) - 1);
                    temp[sizeof(temp) - 1] = '\0';
                    char *unit = strstr(pos, "unit=\"");
                    if (unit) {
                        unit += 6;
                        char *end_unit = strchr(unit, '"');
                        if (end_unit) {
                            *end_unit = '\0';
                            snprintf(info->origin, sizeof(info->origin), "%s %s", temp, unit);
                        }
                    }
                }
            }
        } else if (in_slist_pq && strstr(pos, "<scale")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    char temp[16];
                    *end_value = '\0';
                    strncpy(temp, value, sizeof(temp) - 1);
                    temp[sizeof(temp) - 1] = '\0';
                    char *unit = strstr(pos, "unit=\"");
                    if (unit) {
                        unit += 6;
                        char *end_unit = strchr(unit, '"');
                        if (end_unit) {
                            *end_unit = '\0';
                            snprintf(info->scale, sizeof(info->scale), "%s %s", temp, unit);
                        }
                    }
                }
            }
        } else if (strstr(pos, "<increment")) {
            char *value = strstr(pos, "value=\"");
            if (value) {
                value += 7;
                char *end_value = strchr(value, '"');
                if (end_value) {
                    char temp[16];
                    *end_value = '\0';
                    strncpy(temp, value, sizeof(temp) - 1);
                    temp[sizeof(temp) - 1] = '\0';
                    char *unit = strstr(pos, "unit=\"");
                    if (unit) {
                        unit += 6;
                        char *end_unit = strchr(unit, '"');
                        if (end_unit) {
                            *end_unit = '\0';
                            snprintf(info->increment, sizeof(info->increment), "%s %s", temp, unit);
                        }
                    }
                }
            }
        } else if (in_slist_pq && strstr(pos, "<digits>")) {
            char *start = strstr(pos, "<digits>");
            if (start) {
                start += 8;
                char *end_tag = strstr(start, "</digits>");
                if (end_tag) {
                    *end_tag = '\0';
                    while (*start && isspace((unsigned char)*start)) start++;
                    char *value_end = end_tag - 1;
                    while (value_end > start && isspace((unsigned char)*value_end)) *value_end-- = '\0';
                    char *target = NULL;
                    if (strcmp(current_lead, "MDC_ECG_LEAD_I") == 0) {
                        target = info->lead_i;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_II") == 0) {
                        target = info->lead_ii;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_III") == 0) {
                        target = info->lead_iii;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_AVR") == 0) {
                        target = info->lead_avr;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_AVL") == 0) {
                        target = info->lead_avl;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_AVF") == 0) {
                        target = info->lead_avf;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_V1") == 0) {
                        target = info->lead_v1;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_V2") == 0) {
                        target = info->lead_v2;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_V3") == 0) {
                        target = info->lead_v3;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_V4") == 0) {
                        target = info->lead_v4;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_V5") == 0) {
                        target = info->lead_v5;
                    } else if (strcmp(current_lead, "MDC_ECG_LEAD_V6") == 0) {
                        target = info->lead_v6;
                    }
                    if (target) {
                        strncpy(target, start, 4095);
                        target[4095] = '\0';
                    }
                }
            }
        }
    }

    fclose(fp);
}

static int console_read(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Storage exposed over USB. Application can't read from storage.");
        return -1;
    }

    // Find the latest ecg_archive folder
    DIR *dir = opendir(BASE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s", BASE_PATH);
        return -1;
    }

    struct dirent *entry;
    char latest_folder[256] = "";
    int max_index = -1;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && strncmp(entry->value, "ecg_archive", 11) == 0) {
            if (strcmp(entry->value, "ecg_archive") == 0) {
                if (max_index < 0) {
                    strcpy(latest_folder, entry->value);
                    max_index = 0;
                }
            } else {
                int index = atoi(entry->value, +11); // Skip "ecg_archive_"
                if (index > max_index) {
                    max_index = index;
                    strcpy(latest_folder, entry->value);
                }
            }
        }
    }
    closedir(dir);

    if (max_index == -1 && strlen(latest_folder) == 0) {
        ESP_LOGE(TAG, "No ecg_archive folder found in %s", BASE_PATH);
        return -1;
    }

    // Find XML file in the latest folder
    char folder_path[512];
    snprintf(folder_path, sizeof(folder_path), "%s/%s", BASE_PATH, latest_folder);
    DIR *subdir = opendir(folder_path);
    if (!subdir) {
        ESP_LOGE(TAG, "Failed to open folder %s", folder_path);
        return -1;
    }

    char xml_file[256] = "";
    while ((entry = readdir(subdir)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->value, ".XML")) {
            strcpy(xml_file, entry->value);
            break; // Take the first XML file
        }
    }
    closedir(subdir);

    if (strlen(xml_file) == 0) {
        ESP_LOGE(TAG, "No XML files found in %s", folder_path);
        return -1;
    }

    // Parse the XML file
    char file_path[768];
    snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, xml_file);
    ESP_LOGI(TAG, "Latest folder: %s", latest_folder);
    ESP_LOGI(TAG, "Reading XML file: %s", xml_file);
    patient_info_t info;
    parse_xml_file(file_path, &info);

    // Print patient information
    printf("\n=== Informasi Pasien ===\n");
    printf("Nama Pasien     : %s\n", info.name);
    printf("Jenis Kelamin   : %s\n", info.sex);
    printf("Umur            : %s tahun\n", info.age);
    printf("Tanggal Lahir   : %s\n", info.birth_time);
    printf("Room ID         : %s\n", info.sickroomid);
    printf("Bed ID          : %s\n", info.bedid);
    printf("Inhospital ID   : %s\n", info.inhospital_id);
    printf("Operator       : %s\n", info.cop);
    printf("Waktu Awal      : %s\n", info.effective_time_low);
    printf("Waktu Akhir     : %s\n", info.effective_time_high);
    printf("Case ID         : %s\n", info.casesize);
    printf("Filter          : %s\n", info.filter);
    printf("Kode Unik       : %s\n", info.uniquecode);
    printf("=======================\n");

    // Print EKG interpretation
    printf("\n=== Hasil Interpretasi EKG ===\n");
    printf("Heart Rate      : %s\n", info.hr);
    printf("Interval PR     : %s\n", info.pr_intervals);
    printf("Durasi P      : %s\n", info.p_duration);
    printf("Durasi QRS      : %s\n", info.qrs_duration);
    printf("Durasi T        : %s\n", info.t_duration);
    printf("Durasi QT       : %s\n", info.qt_intervals);
    printf("QT Koreksi      : %s\n", info.qtc_intervals);
    printf("Sudut Axis P    : %s\n", info.p_axis);
    printf("Sudut Axis QRS  : %s\n", info.qrs_axis);
    printf("Sudut Axis T    : %s\n", info.t_axis);
    printf("Amplitudo R V5  : %s\n", info.r_v5);
    printf("Amplitudo S V1  : %s\n", info.s_v1);
    printf("Interpretasi    : %s\n", info.interpretation);
    printf("=== Data Grafik EKG ===\n");
    printf("Lead I          : %s\n", info.lead_i);
    printf("Lead II         : %s\n", info.lead_ii);
    printf("Lead III        : %s\n", info.lead_iii);
    printf("Lead aVR        : %s\n", info.lead_avr);
    printf("Lead aVL        : %s\n", info.lead_avl);
    printf("Lead aVF        : %s\n", info.lead_avf);
    printf("Lead V1         : %s\n", info.lead_v1);
    printf("Lead V2         : %s\n", info.lead_v2);
    printf("Lead V3         : %s\n", info.lead_v3);
    printf("Lead V4         : %s\n", info.lead_v4);
    printf("Lead V5         : %s\n", info.lead_v5);
    printf("Lead V6         : %s\n", info.lead_v6);
    printf("Origin          : %s\n", info.origin);
    printf("Scale           : %s\n", info.scale);
    printf("Increment       : %s\n", info.increment);
    printf("==============================\n");

    return 0;
}

    static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    ESP_LOGI(TAG, "Storage mounted to application: %s", event->mount_changed_data.is_mounted ? "Yes" : "No");
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

    // Mount storage
    _mount();

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
    repl_config.task_stack_size = 8192;
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
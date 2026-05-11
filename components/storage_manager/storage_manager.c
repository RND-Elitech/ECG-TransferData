#include "storage_manager.h"

#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_check.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "sdkconfig.h"

#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
#include "wear_levelling.h"
#include "esp_partition.h"
#else
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#ifdef CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#endif

static const char *TAG = "storage_manager";

#define STORAGE_BASE_PATH "/data"

/* ─── Descriptor USB (MSC) ─── */
#define EPNUM_MSC           1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
enum {
    EDPT_CTRL_OUT = 0x00,
    EDPT_CTRL_IN  = 0x80,
    EDPT_MSC_OUT  = 0x01,
    EDPT_MSC_IN   = 0x81,
};

static tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,  // Espressif VID
    .idProduct          = 0x4002,
    .bcdDevice          = 0x100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static uint8_t const s_fs_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0,
};
static uint8_t const s_hs_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};
#endif

static char const *s_string_desc[] = {
    (const char[]){ 0x09, 0x04 },  // 0: English
    "Espressif",                    // 1: Manufacturer
    "ECG Dongle",                   // 2: Product
    "ECG-B0001",                    // 3: Serial
    "ECG MSC",                      // 4: MSC Interface
};

/* ─── Callback mount changed ─── */
static void _on_mount_changed(tinyusb_msc_event_t *event)
{
    ESP_LOGI(TAG, "Storage mounted to app: %s",
             event->mount_changed_data.is_mounted ? "Ya" : "Tidak");
}

/* ─── Init media penyimpanan ─── */
#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
static esp_err_t _init_spiflash(void)
{
    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (!part) {
        ESP_LOGE(TAG, "Partisi FAT tidak ditemukan");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(wl_mount(part, &wl_handle), TAG, "wl_mount gagal");

    const tinyusb_msc_spiflash_config_t cfg = {
        .wl_handle              = wl_handle,
        .callback_mount_changed = _on_mount_changed,
        .mount_config.max_files = 5,
        .mount_config.format_if_mount_failed = true,
        .mount_config.allocation_unit_size = 512,
    };
    return tinyusb_msc_storage_init_spiflash(&cfg);
}
#else
static esp_err_t _init_sdmmc(void)
{
    static sdmmc_card_t *card = NULL;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID,
    };
    sd_pwr_ctrl_handle_t pwr = NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr),
                        TAG, "LDO pwr ctrl gagal");
    host.pwr_ctrl_handle = pwr;
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
    slot.width = 4;
#else
    slot.width = 1;
#endif
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot.clk = CONFIG_EXAMPLE_PIN_CLK;
    slot.cmd = CONFIG_EXAMPLE_PIN_CMD;
    slot.d0  = CONFIG_EXAMPLE_PIN_D0;
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
    slot.d1  = CONFIG_EXAMPLE_PIN_D1;
    slot.d2  = CONFIG_EXAMPLE_PIN_D2;
    slot.d3  = CONFIG_EXAMPLE_PIN_D3;
#endif
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    card = malloc(sizeof(sdmmc_card_t));
    if (!card) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK((*host.init)());
    ESP_ERROR_CHECK(sdmmc_host_init_slot(host.slot, &slot));

    while (sdmmc_card_init(&host, card)) {
        ESP_LOGE(TAG, "SD card tidak terdeteksi. Coba lagi dalam 3 detik...");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    sdmmc_card_print_info(stdout, card);

    const tinyusb_msc_sdmmc_config_t cfg = {
        .card                   = card,
        .callback_mount_changed = _on_mount_changed,
        .mount_config.max_files = 5,
    };
    return tinyusb_msc_storage_init_sdmmc(&cfg);
}
#endif

/* ─── Public API ─── */

esp_err_t storage_manager_init(void)
{
#ifdef CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH
    ESP_RETURN_ON_ERROR(_init_spiflash(), TAG, "Inisialisasi SPI Flash gagal");
#else
    ESP_RETURN_ON_ERROR(_init_sdmmc(), TAG, "Inisialisasi SDMMC gagal");
#endif
    ESP_ERROR_CHECK(tinyusb_msc_register_callback(
        TINYUSB_MSC_EVENT_MOUNT_CHANGED, _on_mount_changed));

    /* Pasang USB MSC driver */
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = &s_device_desc,
        .string_descriptor        = s_string_desc,
        .string_descriptor_count  = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy             = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = s_fs_desc,
        .hs_configuration_descriptor = s_hs_desc,
        .qualifier_descriptor        = &s_qualifier,
#else
        .configuration_descriptor = s_fs_desc,
#endif
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB install gagal");

    ESP_LOGI(TAG, "Storage manager siap (USB MSC aktif)");
    return ESP_OK;
}

esp_err_t storage_manager_mount(void)
{
    ESP_LOGI(TAG, "Mounting storage ke aplikasi (%s)...", STORAGE_BASE_PATH);
    return tinyusb_msc_storage_mount(STORAGE_BASE_PATH);
}

esp_err_t storage_manager_expose_to_usb(void)
{
    ESP_LOGI(TAG, "Menyerahkan storage ke USB Host...");
    return tinyusb_msc_storage_unmount();
}

bool storage_manager_in_use_by_usb(void)
{
    return tinyusb_msc_storage_in_use_by_usb_host();
}

void storage_manager_cleanup(void)
{
    ESP_LOGI(TAG, "Membersihkan storage awal...");
    DIR *dir = opendir(STORAGE_BASE_PATH);
    if (!dir) {
        ESP_LOGW(TAG, "Gagal membuka direktori untuk dibersihkan: %s", STORAGE_BASE_PATH);
        return;
    }

    struct dirent *entry;
    char path[512];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", STORAGE_BASE_PATH, entry->d_name);

        if (entry->d_type == DT_DIR) {
            DIR *sub = opendir(path);
            if (sub) {
                struct dirent *sub_entry;
                char sub_path[768];
                while ((sub_entry = readdir(sub)) != NULL) {
                    if (strcmp(sub_entry->d_name, ".") == 0 || strcmp(sub_entry->d_name, "..") == 0) continue;
                    snprintf(sub_path, sizeof(sub_path), "%s/%s", path, sub_entry->d_name);
                    unlink(sub_path);
                }
                closedir(sub);
            }
            rmdir(path);
        } else {
            unlink(path);
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Pembersihan storage selesai");
}

uint32_t storage_manager_get_sector_count(void)
{
    return tinyusb_msc_storage_get_sector_count();
}

uint32_t storage_manager_get_sector_size(void)
{
    return tinyusb_msc_storage_get_sector_size();
}

// storage_manager.c
#include "storage_manager.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "wear_levelling.h"
#include <dirent.h>
#include <string.h>
#include <unistd.h>

/* SD card via built-in SDMMC peripheral */
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "vfs_fat_internal.h"
#include "device_info.h"


static const char *TAG = "storage_manager";

#define STORAGE_BASE_PATH "/data"

/* Jumlah percobaan deteksi SD card sebelum fallback ke flash */
#define SDCARD_MAX_RETRY  3

/* Status media yang sedang aktif */
static bool s_using_sdcard = false;

/* ─── Descriptor USB (MSC) ─── */
#define EPNUM_MSC 1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
enum {
  EDPT_CTRL_OUT = 0x00,
  EDPT_CTRL_IN = 0x80,
  EDPT_MSC_OUT = 0x01,
  EDPT_MSC_IN = 0x81,
};

static tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
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
    .bNumConfigurations = 0x01,
};

static uint8_t const s_fs_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};
static uint8_t const s_hs_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};
#endif

static char s_serial_number[32] = "ECG-B0001"; // Fallback awal

static char const *s_string_desc[] = {
    (const char[]){0x09, 0x04}, // 0: English
    "Espressif",                // 1: Manufacturer
    "ECG Dongle",               // 2: Product
    s_serial_number,            // 3: Serial (Dinamis dari NVS/MAC)
    "ECG MSC",                  // 4: MSC Interface
};

/* ─── Callback mount changed ─── */
static void _on_mount_changed(tinyusb_msc_event_t *event) {
  ESP_LOGI(TAG, "Storage mounted to app: %s",
           event->mount_changed_data.is_mounted ? "Ya" : "Tidak");
}

/* ─── Init SD Card via built-in SDMMC (percobaan terbatas, lalu return error) ─── */
static esp_err_t _init_sdmmc(void) {
  static sdmmc_card_t *card = NULL;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;  // 1-bit mode

  /* Pin assignment: Fallback to manual values if Kconfig is not updated */
#ifndef CONFIG_EXAMPLE_PIN_CLK
#define CONFIG_EXAMPLE_PIN_CLK 39
#endif
#ifndef CONFIG_EXAMPLE_PIN_CMD
#define CONFIG_EXAMPLE_PIN_CMD 38
#endif
#ifndef CONFIG_EXAMPLE_PIN_D0
#define CONFIG_EXAMPLE_PIN_D0  40
#endif

  slot.clk = CONFIG_EXAMPLE_PIN_CLK;
  slot.cmd = CONFIG_EXAMPLE_PIN_CMD;
  slot.d0  = CONFIG_EXAMPLE_PIN_D0;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  card = malloc(sizeof(sdmmc_card_t));
  if (!card)
    return ESP_ERR_NO_MEM;

  esp_err_t ret = (*host.init)();
  if (ret != ESP_OK) {
    free(card);
    card = NULL;
    return ret;
  }

  ret = sdmmc_host_init_slot(host.slot, &slot);
  if (ret != ESP_OK) {
    (*host.deinit)();
    free(card);
    card = NULL;
    return ret;
  }

  /* Coba inisialisasi SD card dengan jumlah percobaan terbatas */
  int retry = 0;
  while ((ret = sdmmc_card_init(&host, card)) != ESP_OK) {
    retry++;
    if (retry >= SDCARD_MAX_RETRY) {
      ESP_LOGW(TAG, "SD card tidak terdeteksi setelah %d percobaan. "
                    "Beralih ke SPI Flash internal...", SDCARD_MAX_RETRY);
      (*host.deinit)();
      free(card);
      card = NULL;
      return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "SD card tidak terdeteksi (percobaan %d/%d). "
                  "Coba lagi dalam 1 detik...", retry, SDCARD_MAX_RETRY);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  sdmmc_card_print_info(stdout, card);

  const tinyusb_msc_sdmmc_config_t cfg = {
      .card                   = card,
      .callback_mount_changed = _on_mount_changed,
      .mount_config.max_files = 5,
  };
  return tinyusb_msc_storage_init_sdmmc(&cfg);
}

/* ─── Init SPI Flash internal sebagai fallback ─── */
static esp_err_t _init_spiflash(void) {
  ESP_LOGI(TAG, "Menggunakan SPI Flash internal sebagai storage...");

  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
  if (!part) {
    ESP_LOGE(TAG, "Partisi 'storage' (FAT) tidak ditemukan di tabel partisi!");
    return ESP_ERR_NOT_FOUND;
  }

  static wl_handle_t wl_handle = WL_INVALID_HANDLE;
  esp_err_t ret = wl_mount(part, &wl_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "wl_mount gagal: %s", esp_err_to_name(ret));
    return ret;
  }

  const tinyusb_msc_spiflash_config_t cfg = {
      .wl_handle              = wl_handle,
      .callback_mount_changed = _on_mount_changed,
      .mount_config.max_files = 5,
  };

  ret = tinyusb_msc_storage_init_spiflash(&cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "tinyusb_msc_storage_init_spiflash gagal: %s", esp_err_to_name(ret));
    wl_unmount(wl_handle);
    return ret;
  }

  ESP_LOGI(TAG, "SPI Flash internal siap (partisi: %s, ukuran: %"PRIu32" KB)",
           part->label, part->size / 1024);
  return ESP_OK;
}


/* ─── Public API ─── */

static bool s_storage_initialized = false;

esp_err_t storage_manager_init(void) {
  if (s_storage_initialized) {
    ESP_LOGI(TAG, "Storage manager sudah terinisialisasi.");
    return ESP_OK;
  }

  /* Dapatkan SN perangkat secara dinamis */
  device_info_get_sn(s_serial_number, sizeof(s_serial_number));

  /* Coba SD card terlebih dahulu; jika tidak ada, fallback ke SPI Flash */
  esp_err_t ret = _init_sdmmc();
  if (ret == ESP_OK) {
    s_using_sdcard = true;
    ESP_LOGI(TAG, "Storage: menggunakan SD Card");
  } else {
    ESP_LOGW(TAG, "SD Card tidak tersedia (err: %s), beralih ke SPI Flash...",
             esp_err_to_name(ret));
    ret = _init_spiflash();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Inisialisasi SPI Flash juga gagal: %s", esp_err_to_name(ret));
      return ret;
    }
    s_using_sdcard = false;
    ESP_LOGI(TAG, "Storage: menggunakan SPI Flash internal");
  }

  ESP_RETURN_ON_ERROR(tinyusb_msc_register_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED,
                                                     _on_mount_changed),
                      TAG, "Register callback gagal");

  /* Pasang USB MSC driver */
  const tinyusb_config_t tusb_cfg = {
      .device_descriptor = &s_device_desc,
      .string_descriptor = s_string_desc,
      .string_descriptor_count =
          sizeof(s_string_desc) / sizeof(s_string_desc[0]),
      .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
      .fs_configuration_descriptor = s_fs_desc,
      .hs_configuration_descriptor = s_hs_desc,
      .qualifier_descriptor = &s_qualifier,
#else
      .configuration_descriptor = s_fs_desc,
#endif
  };
  ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG,
                      "TinyUSB install gagal");

  ESP_LOGI(TAG, "Storage manager siap (USB MSC aktif, media: %s)",
           s_using_sdcard ? "SD Card" : "SPI Flash");
  s_storage_initialized = true;
  return ESP_OK;
}

esp_err_t storage_manager_mount(void) {
  ESP_LOGI(TAG, "Mounting storage ke aplikasi (%s)...", STORAGE_BASE_PATH);
  return tinyusb_msc_storage_mount(STORAGE_BASE_PATH);
}

esp_err_t storage_manager_expose_to_usb(void) {
  ESP_LOGI(TAG, "Menyerahkan storage ke USB Host...");
  return tinyusb_msc_storage_unmount();
}

bool storage_manager_in_use_by_usb(void) {
  return tinyusb_msc_storage_in_use_by_usb_host();
}


uint32_t storage_manager_get_sector_count(void) {
  return tinyusb_msc_storage_get_sector_count();
}

uint32_t storage_manager_get_sector_size(void) {
  return tinyusb_msc_storage_get_sector_size();
}

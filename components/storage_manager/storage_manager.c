// storage_manager.c
#include "storage_manager.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include <dirent.h>
#include <string.h>
#include <unistd.h>

/* SD card via built-in SDMMC peripheral */
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"


static const char *TAG = "storage_manager";

#define STORAGE_BASE_PATH "/data"

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

static char const *s_string_desc[] = {
    (const char[]){0x09, 0x04}, // 0: English
    "Espressif",                // 1: Manufacturer
    "ECG Dongle",               // 2: Product
    "ECG-B0001",                // 3: Serial
    "ECG MSC",                  // 4: MSC Interface
};

/* ─── Callback mount changed ─── */
static void _on_mount_changed(tinyusb_msc_event_t *event) {
  ESP_LOGI(TAG, "Storage mounted to app: %s",
           event->mount_changed_data.is_mounted ? "Ya" : "Tidak");
}

/* ─── Init SD Card via built-in SDMMC ─── */
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


/* ─── Public API ─── */

esp_err_t storage_manager_init(void) {
  ESP_RETURN_ON_ERROR(_init_sdmmc(), TAG, "Inisialisasi SDMMC gagal");
  ESP_ERROR_CHECK(tinyusb_msc_register_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED,
                                                _on_mount_changed));

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

  ESP_LOGI(TAG, "Storage manager siap (USB MSC aktif)");
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

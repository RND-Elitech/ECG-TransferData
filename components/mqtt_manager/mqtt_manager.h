#pragma once

#include "esp_err.h"
#include "stdbool.h"

/**
 * @brief Callback yang dipanggil saat perintah upload diterima dari broker MQTT.
 */
typedef void (*mqtt_on_upload_cmd_cb_t)(void *ctx);

/**
 * @brief Callback yang dipanggil saat perintah format diterima dari broker MQTT.
 */
typedef void (*mqtt_on_format_cmd_cb_t)(void *ctx);

/**
 * @brief Konfigurasi untuk mqtt_manager.
 */
typedef struct {
    const char              *broker_uri;      ///< URI broker (contoh: "mqtts://host.com")
    int                      broker_port;     ///< Port broker MQTT
    const char              *username;        ///< Username autentikasi MQTT
    const char              *password;        ///< Password autentikasi MQTT
    const char              *gateway_sn;      ///< Serial Number perangkat
    mqtt_on_upload_cmd_cb_t  on_upload_cmd;   ///< Callback ketika perintah upload diterima
    mqtt_on_format_cmd_cb_t  on_format_cmd;   ///< Callback ketika perintah format diterima
    void                    *cb_ctx;          ///< Konteks yang diteruskan ke callback
} mqtt_manager_config_t;

/**
 * @brief Inisialisasi dan mulai MQTT client.
 *
 * @param cfg Konfigurasi MQTT
 * @return ESP_OK jika berhasil
 */
esp_err_t mqtt_manager_init(const mqtt_manager_config_t *cfg);

/**
 * @brief Publish status upload (berhasil / gagal) ke broker.
 *
 * @param success true jika upload berhasil, false jika gagal
 */
void mqtt_manager_publish_upload_status(bool success);

/**
 * @brief Publish status format (berhasil / gagal) ke broker.
 *
 * @param success true jika format berhasil, false jika gagal
 */
void mqtt_manager_publish_format_status(bool success);

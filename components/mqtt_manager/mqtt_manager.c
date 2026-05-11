#include "mqtt_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "mqtt_manager";

/* State internal modul */
static esp_mqtt_client_handle_t  s_client      = NULL;
static mqtt_manager_config_t     s_cfg         = {0};
static char                      s_gateway_sn[32] = {0};

/* ─── Helper: publish dengan format topik standar ─── */
static int _publish(const char *subtopic, const char *payload, int qos, int retain)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "iotgateway/%s/dongle/%s", s_gateway_sn, subtopic);
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
    ESP_LOGI(TAG, "Publish → %s (msg_id=%d)", topic, msg_id);
    return msg_id;
}

/* ─── Helper: subscribe dengan format topik standar ─── */
static void _subscribe(const char *subtopic)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "iotgateway/%s/dongle/%s", s_gateway_sn, subtopic);
    esp_mqtt_client_subscribe(s_client, topic, 0);
    ESP_LOGI(TAG, "Subscribe ← %s", topic);
}

/* ─── Helper: cocokkan topik yang diterima ─── */
static bool _topic_matches(const char *received, int len, const char *subtopic)
{
    char expected[128];
    snprintf(expected, sizeof(expected), "iotgateway/%s/dongle/%s", s_gateway_sn, subtopic);
    return (len == (int)strlen(expected)) && (strncmp(received, expected, len) == 0);
}

/* ─── Publish payload info perangkat ─── */
static void _publish_device_info(void)
{
    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"gateway_sn\":\"%s\",\"data\":{"
        "\"model\":\"EcgDongle-01\","
        "\"firmware_version\":\"1.1.1\","
        "\"hardware_revision\":\"R1.1\","
        "\"build_date\":\"2026-05-08\"}}",
        s_gateway_sn);
    _publish("info", payload, 1, 1);
}

/* ─── Publish IP dan fungsi perangkat ─── */
static void _publish_device_function(void)
{
    char ip_str[16] = "0.0.0.0";
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }
    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"gateway_sn\":\"%s\",\"data\":{\"ip\":\"%s\",\"device_function\":\"ecg1200g\"}}",
        s_gateway_sn, ip_str);
    _publish("function", payload, 1, 1);
}

/* ─── Publish IP saja (respons get) ─── */
static void _publish_ip(void)
{
    char ip_str[16] = "0.0.0.0";
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }
    char payload[128];
    snprintf(payload, sizeof(payload),
        "{\"gateway_sn\":\"%s\",\"data\":{\"ip\":\"%s\"}}", s_gateway_sn, ip_str);
    _publish("ip", payload, 1, 0);
}

/* ─── Event handler MQTT ─── */
static void _mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "Terhubung ke broker MQTT");

            /* Subscribe topik-topik yang dibutuhkan */
            _subscribe("upload");

            _subscribe("ip/get");

            /* Publish status online */
            char payload[128];
            snprintf(payload, sizeof(payload),
                "{\"gateway_sn\":\"%s\",\"data\":{\"online\":true}}", s_gateway_sn);
            _publish("status/online", payload, 1, 1);

            /* Publish info dan fungsi perangkat */
            _publish_device_info();
            _publish_device_function();
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Terputus dari broker MQTT");
            break;

        case MQTT_EVENT_DATA: {
            const char *topic = event->topic;
            int topic_len     = event->topic_len;

            /* Perintah upload */
            if (_topic_matches(topic, topic_len, "upload")) {
                char *data = malloc(event->data_len + 1);
                if (!data) break;
                memcpy(data, event->data, event->data_len);
                data[event->data_len] = '\0';

                bool valid = strstr(data, s_gateway_sn)
                          && strstr(data, "\"command\"")
                          && strstr(data, "\"upload\"");
                free(data);

                if (valid) {
                    ESP_LOGI(TAG, "Perintah upload valid diterima, memanggil callback...");
                    if (s_cfg.on_upload_cmd) {
                        s_cfg.on_upload_cmd(s_cfg.cb_ctx);
                    }
                } else {
                    ESP_LOGW(TAG, "Payload upload tidak valid atau SN tidak cocok");
                }
            }

            /* Permintaan IP */
            else if (_topic_matches(topic, topic_len, "ip/get")) {
                if (event->data_len == 3 && strncmp(event->data, "get", 3) == 0) {
                    _publish_ip();
                }
            }
            break;
        }

        default:
            break;
    }
}

/* ─── Public API ─── */

esp_err_t mqtt_manager_init(const mqtt_manager_config_t *cfg)
{
    s_cfg = *cfg;
    strncpy(s_gateway_sn, cfg->gateway_sn, sizeof(s_gateway_sn) - 1);

    /* Konfigurasi LWT */
    static char lwt_topic[128];
    static char lwt_payload[128];
    snprintf(lwt_topic,   sizeof(lwt_topic),
             "iotgateway/%s/dongle/status/online", s_gateway_sn);
    snprintf(lwt_payload, sizeof(lwt_payload),
             "{\"gateway_sn\":\"%s\",\"data\":{\"online\":false}}", s_gateway_sn);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri  = cfg->broker_uri,
            .address.port = cfg->broker_port,
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .username                  = cfg->username,
            .authentication.password   = cfg->password,
        },
        .session.last_will = {
            .topic   = lwt_topic,
            .msg     = lwt_payload,
            .msg_len = 0,
            .qos     = 1,
            .retain  = 1,
        },
        .task = {
            .stack_size = 10240,
            .priority   = 5,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Gagal inisialisasi MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, _mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client dimulai → %s:%d", cfg->broker_uri, cfg->broker_port);
    return ESP_OK;
}

void mqtt_manager_publish_upload_status(bool success)
{
    char payload[128];
    snprintf(payload, sizeof(payload),
        "{\"gateway_sn\":\"%s\",\"data\":{\"status\":\"%s\"}}",
        s_gateway_sn, success ? "completed" : "failed");
    _publish("upload/status", payload, 1, 0);
}



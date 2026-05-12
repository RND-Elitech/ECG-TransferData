/**
 * @file dns_server.c
 * @brief DNS server sederhana untuk Captive Portal.
 *
 * Menjawab SEMUA query DNS dengan IP 192.168.4.1 (ESP32 AP IP).
 * Berjalan sebagai FreeRTOS task terpisah di UDP port 53.
 *
 * Referensi format DNS packet: RFC 1035
 */

#include "dns_server.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

static const char *TAG = "dns_server";
static TaskHandle_t s_dns_task_handle = NULL;

/* ─── DNS Packet Structure ─── */
#define DNS_PORT        53
#define DNS_BUF_SIZE    512

/* Flags untuk DNS response */
#define DNS_FLAG_QR     (1 << 15)  /* Response */
#define DNS_FLAG_AA     (1 << 10)  /* Authoritative */
#define DNS_FLAG_RA     (1 << 7)   /* Recursion Available */

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;   /* Question count */
    uint16_t ancount;   /* Answer count */
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

/* ─── DNS Task ─── */
static void dns_server_task(void *pvParameters) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Gagal buat socket DNS: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* Set socket non-blocking dengan timeout agar task bisa berhenti */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Gagal bind DNS port 53: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server berjalan di port 53");

    static uint8_t buf[DNS_BUF_SIZE];

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&client_addr, &client_len);

        if (len < 0) {
            /* Timeout — cek apakah task harus berhenti */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Cek notifikasi stop dari dns_server_stop() */
                uint32_t notify;
                if (xTaskNotifyWait(0, 0xFFFFFFFF, &notify, 0) == pdTRUE) {
                    break;
                }
                continue;
            }
            ESP_LOGW(TAG, "recvfrom error: errno %d", errno);
            continue;
        }

        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        /* ─── Parse header dari query ─── */
        dns_header_t *req_hdr = (dns_header_t *)buf;
        uint16_t req_id    = req_hdr->id;
        uint16_t req_flags = ntohs(req_hdr->flags);

        /* Abaikan jika bukan query (QR=0 berarti query) */
        if (req_flags & 0x8000) continue;

        /* ─── Build response ─── */
        /*
         * Format response DNS:
         *   [Header][Original Question][Answer RR]
         *
         * Answer RR untuk IPv4 (type A):
         *   Name: pointer ke question (0xC00C = offset 12)
         *   Type: 0x0001 (A)
         *   Class: 0x0001 (IN)
         *   TTL: 60 detik
         *   RDLENGTH: 4
         *   RDATA: 192.168.4.1
         */
        uint8_t resp[DNS_BUF_SIZE];
        memcpy(resp, buf, len);   /* Salin seluruh query sebagai basis */

        dns_header_t *resp_hdr = (dns_header_t *)resp;
        resp_hdr->id      = req_id;
        resp_hdr->flags   = htons(DNS_FLAG_QR | DNS_FLAG_AA | DNS_FLAG_RA);
        resp_hdr->ancount = htons(1);  /* 1 answer */

        /* Append Answer Resource Record setelah question */
        uint8_t *ptr = resp + len;

        /* Name: pointer ke offset 12 (question nama) */
        *ptr++ = 0xC0;
        *ptr++ = 0x0C;

        /* Type: A (0x0001) */
        *ptr++ = 0x00; *ptr++ = 0x01;

        /* Class: IN (0x0001) */
        *ptr++ = 0x00; *ptr++ = 0x01;

        /* TTL: 60 detik */
        *ptr++ = 0x00; *ptr++ = 0x00; *ptr++ = 0x00; *ptr++ = 60;

        /* RDLENGTH: 4 byte (IPv4) */
        *ptr++ = 0x00; *ptr++ = 0x04;

        /* RDATA: 192.168.4.1 */
        *ptr++ = 192; *ptr++ = 168; *ptr++ = 4; *ptr++ = 1;

        int resp_len = (int)(ptr - resp);

        sendto(sock, resp, resp_len, 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    ESP_LOGI(TAG, "DNS server dihentikan");
    close(sock);
    vTaskDelete(NULL);
}

/* ─── Public API ─── */
esp_err_t dns_server_start(void) {
    if (s_dns_task_handle != NULL) {
        ESP_LOGW(TAG, "DNS server sudah berjalan");
        return ESP_OK;
    }

    BaseType_t rc = xTaskCreate(dns_server_task, "dns_server",
                                4096, NULL, 5, &s_dns_task_handle);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat DNS task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dns_server_stop(void) {
    if (s_dns_task_handle) {
        xTaskNotify(s_dns_task_handle, 1, eSetValueWithOverwrite);
        /* Task akan self-delete setelah loop berhenti */
        vTaskDelay(pdMS_TO_TICKS(1500));
        s_dns_task_handle = NULL;
    }
}

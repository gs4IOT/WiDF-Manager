/*  widf_mngr_dns.c — Captive portal DNS server for WIDF Manager */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "widf_mngr_dns.h"

static const char *TAG = "widf_dns";

#define DNS_PORT     53
#define DNS_BUF_SIZE 256

static TaskHandle_t s_dns_task = NULL;
static volatile bool s_dns_stop = false;

static int build_dns_response(uint8_t *buf, int query_len)
{
    if (query_len < 12) return -1;

    buf[2] = 0x81;
    buf[3] = 0x80;
    buf[6] = 0x00;
    buf[7] = 0x01;

    int pos = query_len;
    if (pos + 16 > DNS_BUF_SIZE) return -1;

    buf[pos++] = 0xC0; buf[pos++] = 0x0C;  /* name pointer */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* type A */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* class IN */
    buf[pos++] = 0x00; buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0x3C;  /* TTL 60s */
    buf[pos++] = 0x00; buf[pos++] = 0x04;  /* RDLENGTH */
    buf[pos++] = 192;  buf[pos++] = 168;
    buf[pos++] = 4;    buf[pos++] = 1;     /* 192.168.4.1 */

    return pos;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 200ms receive timeout so the stop flag is checked frequently */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(DNS_PORT),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind port 53");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server listening on port 53");

    uint8_t buf[DNS_BUF_SIZE];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    while (!s_dns_stop) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len > 0) {
            ESP_LOGI(TAG, "DNS query received, %d bytes", len);
            int resp_len = build_dns_response(buf, len);
            if (resp_len > 0) {
                sendto(sock, buf, resp_len, 0,
                       (struct sockaddr *)&client, client_len);
            }
        }
        /* On timeout (len < 0) just loop and recheck s_dns_stop */
    }

    close(sock);
    ESP_LOGI(TAG, "DNS server stopped");
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

void dns_server_start(void)
{
    if (s_dns_task != NULL) return;
    s_dns_stop = false;
    xTaskCreate(dns_task, "dns_server", 4096, NULL, 5, &s_dns_task);
    ESP_LOGI(TAG, "DNS server task started");
}

void dns_server_stop(void)
{
    s_dns_stop = true;
    s_dns_task = NULL;
    ESP_LOGI(TAG, "DNS server stop complete");
}
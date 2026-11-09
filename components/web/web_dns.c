/*
 * Captive-Portal-DNS: beantwortet jede A-Anfrage mit 192.168.4.1.
 * Dadurch öffnen Handys/Laptops nach dem Verbinden mit "LoxPanel-XXXX" die Einrichtungsseite.
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "web_dns";

static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "Port 53 nicht verfügbar");
        if (s >= 0) close(s);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        /* Nur Standard-Abfragen mit genau einer Frage */
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;
        int q = 12;
        while (q < n && buf[q]) q += buf[q] + 1;
        q += 5;                                     // Null-Byte + QTYPE + QCLASS
        if (q > n) continue;
        uint16_t qtype = (buf[q - 4] << 8) | buf[q - 3];

        buf[2] = 0x84 | (buf[2] & 0x01);           // Antwort, autoritativ, RD übernehmen
        buf[3] = 0x80;                             // RA, kein Fehler
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0; // ANCOUNT
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = q;
        if (qtype == 1) {
            const uint8_t ans[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(s, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

void web_dns_start(void)
{
    xTaskCreate(dns_task, "web_dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Captive-Portal-DNS aktiv");
}

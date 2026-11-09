/*
 * Uhrzeit per SNTP (Zeitzone Schweiz/Mitteleuropa).
 * Laut SPEC soll später die Zeit des Miniservers Vorrang haben; SNTP bleibt als Rückfallebene.
 */

#include "net_time.h"

#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "net_time";

void net_time_start(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP nicht gestartet");
    }
}

bool net_time_valid(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_year + 1900 >= 2024;
}

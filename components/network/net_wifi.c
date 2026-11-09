/*
 * WLAN-Verwaltung über ESP32-C6 (esp_hosted + esp_wifi_remote).
 *
 * Alle Entscheidungen (Verbinden, Backoff, Roaming) trifft eine eigene Task.
 * Die Event-Handler legen nur Nachrichten in die Queue. So blockieren die
 * RPC-Aufrufe zum C6 weder die Event-Loop noch den esp_timer-Task (LVGL-Tick).
 */

#include "net_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "net_wifi";

#define BACKOFF_MIN_MS      1000
#define BACKOFF_MAX_MS      15000
#define CONNECT_TIMEOUT_MS  20000   // hängender Verbindungsversuch -> neu anstossen
#define DHCP_TIMEOUT_MS     20000   // verbunden, aber keine IP -> neu verbinden
#define RSSI_POLL_MS        2000
#define ROAM_CHECK_MS       15000
#define ROAM_SCAN_MAX_AP    20
#define TASK_TICK_MS        500
#define HEALTH_CHECK_MS     10000   // Lebenszeichen vom C6 abfragen
#define HEALTH_MAX_FAILS    3       // so viele Fehlversuche in Folge -> Neustart

typedef enum {
    MSG_STA_START,
    MSG_STA_CONNECTED,
    MSG_STA_DISCONNECTED,
    MSG_GOT_IP,
    MSG_LOST_IP,
    MSG_SCAN_DONE,
} msg_type_t;

typedef struct {
    msg_type_t type;
    uint8_t reason;
    uint8_t bssid[6];
    uint8_t channel;
    uint32_t ip;
} msg_t;

static QueueHandle_t s_queue;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static net_wifi_status_t s_status;      // durch s_lock geschützt
static wifi_config_t s_cfg;
static esp_netif_t *s_netif;

static bool s_configured;               // SSID vorhanden
static char s_ap_ssid[24];
static esp_netif_t *s_ap_netif;
static bool s_static_ip;                // feste IP statt DHCP

/* Nur von der Task benutzt */
static bool s_sta_started;
static bool s_link_up;                // mit AP verbunden (unabhängig von IP)
static bool s_connect_pending;          // esp_wifi_connect läuft
static bool s_roaming;                  // Trennung ist gewollt (Wechsel auf neuen AP)
static bool s_scanning;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static int64_t s_next_try_us;
static int64_t s_connect_started_us;
static int64_t s_last_rssi_us;
static int64_t s_last_roam_us;
static int64_t s_last_health_us;
static int64_t s_no_ip_since_us;        // Zeitpunkt seit dem Link ohne IP besteht (0 = n/a)

/* Access Point, der zuletzt keine IP lieferte: wird eine Zeit lang gemieden (Mesh) */
#define BAD_AP_HOLD_MS      (5 * 60 * 1000)
static uint8_t s_bad_bssid[6];
static int64_t s_bad_until_us;
static bool s_ignore_scan_done;         // SCAN_DONE stammt vom eigenen blockierenden Scan
static int s_health_fails;

/* ------------------------------------------------------------------ */

static void set_state(net_wifi_state_t st)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = st;
    portEXIT_CRITICAL(&s_lock);
}

const char *net_wifi_state_text(net_wifi_state_t state)
{
    switch (state) {
    case NET_WIFI_UNCONFIGURED: return "nicht eingerichtet";
    case NET_WIFI_STARTING:     return "startet";
    case NET_WIFI_CONNECTING:   return "verbindet";
    case NET_WIFI_CONNECTED:    return "verbunden";
    case NET_WIFI_DISCONNECTED: return "getrennt";
    case NET_WIFI_SETUP_AP:     return "Einrichtung";
    }
    return "?";
}

void net_wifi_get_status(net_wifi_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
    if (out->state == NET_WIFI_DISCONNECTED) {
        int64_t left = (s_next_try_us - esp_timer_get_time()) / 1000;
        out->retry_in_ms = left > 0 ? (uint32_t)left : 0;
    } else {
        out->retry_in_ms = 0;
    }
}

bool net_wifi_is_connected(void)
{
    portENTER_CRITICAL(&s_lock);
    bool c = s_status.state == NET_WIFI_CONNECTED;
    portEXIT_CRITICAL(&s_lock);
    return c;
}

/* ------------------------------------------------------------------ */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    msg_t m = { 0 };
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            m.type = MSG_STA_START;
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *ev = data;
            m.type = MSG_STA_CONNECTED;
            memcpy(m.bssid, ev->bssid, 6);
            m.channel = ev->channel;
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev = data;
            m.type = MSG_STA_DISCONNECTED;
            m.reason = ev->reason;
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            m.type = MSG_SCAN_DONE;
            break;
        default:
            return;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *ev = data;
            m.type = MSG_GOT_IP;
            m.ip = ev->ip_info.ip.addr;
        } else if (id == IP_EVENT_STA_LOST_IP) {
            m.type = MSG_LOST_IP;
        } else {
            return;
        }
    } else {
        return;
    }
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue voll, Ereignis %d verworfen", (int)m.type);
    }
}

/* ------------------------------------------------------------------ */

/*
 * Gibt es einen gemiedenen AP, gezielt den besten anderen AP derselben SSID wählen.
 * Gibt es keinen anderen, bleibt die Auswahl frei (lieber der schlechte AP als gar keiner).
 */
static void pick_ap_avoiding_bad(void)
{
    if (s_bad_until_us == 0 || esp_timer_get_time() > s_bad_until_us) {
        s_bad_until_us = 0;
        return;
    }
    wifi_scan_config_t sc = { .ssid = s_cfg.sta.ssid, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    s_ignore_scan_done = true;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        s_ignore_scan_done = false;
        return;
    }
    uint16_t n = ROAM_SCAN_MAX_AP;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (strcmp((const char *)recs[i].ssid, (const char *)s_cfg.sta.ssid) != 0) continue;
            if (memcmp(recs[i].bssid, s_bad_bssid, 6) == 0) continue;
            if (best < 0 || recs[i].rssi > recs[best].rssi) best = i;
        }
        if (best >= 0) {
            ESP_LOGI(TAG, "Meide " MACSTR ", versuche " MACSTR " (Kanal %d, %d dBm)",
                     MAC2STR(s_bad_bssid), MAC2STR(recs[best].bssid), recs[best].primary, recs[best].rssi);
            memcpy(s_cfg.sta.bssid, recs[best].bssid, 6);
            s_cfg.sta.bssid_set = 1;
            s_cfg.sta.channel = recs[best].primary;
            esp_wifi_set_config(WIFI_IF_STA, &s_cfg);
        } else {
            ESP_LOGW(TAG, "Kein anderer Access Point sichtbar");
        }
    }
    free(recs);
}

static void start_connect(void)
{
    pick_ap_avoiding_bad();
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        s_connect_pending = true;
        s_connect_started_us = esp_timer_get_time();
        set_state(NET_WIFI_CONNECTING);
    } else {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        s_next_try_us = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
        set_state(NET_WIFI_DISCONNECTED);
    }
}

/* Feste BSSID wieder lösen: beim normalen Reconnect den stärksten AP nehmen. */
static void clear_bssid_lock(void)
{
    if (s_cfg.sta.bssid_set) {
        s_cfg.sta.bssid_set = 0;
        s_cfg.sta.channel = 0;
        esp_wifi_set_config(WIFI_IF_STA, &s_cfg);
    }
}

static void schedule_retry(void)
{
    s_next_try_us = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
    ESP_LOGI(TAG, "Neuer Versuch in %u ms", (unsigned)s_backoff_ms);
    s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
    set_state(NET_WIFI_DISCONNECTED);
}

static void update_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.rssi = ap.rssi;
        s_status.channel = ap.primary;
        memcpy(s_status.bssid, ap.bssid, 6);
        portEXIT_CRITICAL(&s_lock);
    }
}

static void roam_check(void)
{
    int8_t rssi;
    portENTER_CRITICAL(&s_lock);
    rssi = s_status.rssi;
    portEXIT_CRITICAL(&s_lock);

    if (rssi == 0 || rssi >= CONFIG_LOXPANEL_WIFI_ROAM_RSSI || s_scanning) {
        return;
    }
    wifi_scan_config_t sc = {
        .ssid = s_cfg.sta.ssid,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        s_scanning = true;
        ESP_LOGI(TAG, "Signal schwach (%d dBm), suche besseren Access Point", rssi);
    }
}

static void roam_evaluate(void)
{
    s_scanning = false;
    uint16_t n = ROAM_SCAN_MAX_AP;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        return;
    }

    net_wifi_status_t cur;
    net_wifi_get_status(&cur);

    int best = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp((const char *)recs[i].ssid, (const char *)s_cfg.sta.ssid) != 0) continue;
        if (memcmp(recs[i].bssid, cur.bssid, 6) == 0) continue;
        if (best < 0 || recs[i].rssi > recs[best].rssi) best = i;
    }

    if (best >= 0 && s_link_up && recs[best].rssi >= cur.rssi + CONFIG_LOXPANEL_WIFI_ROAM_GAIN) {
        ESP_LOGI(TAG, "Wechsel auf " MACSTR " (Kanal %d, %d dBm statt %d dBm)",
                 MAC2STR(recs[best].bssid), recs[best].primary, recs[best].rssi, cur.rssi);
        memcpy(s_cfg.sta.bssid, recs[best].bssid, 6);
        s_cfg.sta.bssid_set = 1;
        s_cfg.sta.channel = recs[best].primary;
        esp_wifi_set_config(WIFI_IF_STA, &s_cfg);
        s_roaming = true;
        esp_wifi_disconnect();
    } else {
        ESP_LOGI(TAG, "Kein deutlich besserer Access Point gefunden");
    }
    free(recs);
}

static void read_c6_version(void)
{
    esp_hosted_coprocessor_fwver_t v = { 0 };
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        snprintf(s_status.c6_fw, sizeof(s_status.c6_fw), "%u.%u.%u",
                 (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "ESP32-C6 Firmware %u.%u.%u", (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
    } else {
        ESP_LOGW(TAG, "C6-Firmware-Version nicht lesbar");
    }
}

/* Einmaliger Netztest nach Erhalt der IP: Gateway erreichbar? Namensauflösung? */
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

static void ping_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t sent = 0, recv = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    ESP_LOGI(TAG, "Netztest: Ping Gateway %u/%u Antworten", (unsigned)recv, (unsigned)sent);
    esp_ping_delete_session(hdl);
}

static void net_selftest_task(void *arg)
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.gw.addr) {
        esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
        cfg.target_addr.type = IPADDR_TYPE_V4;
        cfg.target_addr.u_addr.ip4.addr = ip.gw.addr;
        cfg.count = 4;
        esp_ping_callbacks_t cbs = { .on_ping_end = ping_end };
        esp_ping_handle_t h;
        if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) esp_ping_start(h);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
    struct addrinfo *res = NULL;
    struct addrinfo hints = { .ai_family = AF_INET };
    int rc = getaddrinfo("pool.ntp.org", NULL, &hints, &res);
    if (rc == 0 && res) {
        char a[16];
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, a, sizeof(a));
        ESP_LOGI(TAG, "Netztest: pool.ntp.org = %s", a);
        freeaddrinfo(res);
    } else {
        ESP_LOGW(TAG, "Netztest: Namensauflösung fehlgeschlagen (%d)", rc);
    }
    vTaskDelete(NULL);
}

static void handle_msg(const msg_t *m)
{
    switch (m->type) {
    case MSG_STA_START:
        /* esp_hosted meldet STA_START teils doppelt; ein zweiter connect() bricht den ersten ab */
        if (s_sta_started) {
            break;
        }
        s_sta_started = true;
        read_c6_version();
        {
            uint8_t sta_mac[6];
            uint8_t netif_mac[6] = { 0 };
            esp_netif_get_mac(s_netif, netif_mac);
            if (esp_wifi_get_mac(WIFI_IF_STA, sta_mac) == ESP_OK) {
                ESP_LOGI(TAG, "WLAN-MAC (C6) " MACSTR ", Netzwerk-Stack (P4) " MACSTR,
                         MAC2STR(sta_mac), MAC2STR(netif_mac));
            }
        }
        if (s_configured) {
            start_connect();
        } else {
            set_state(s_ap_netif ? NET_WIFI_SETUP_AP : NET_WIFI_UNCONFIGURED);
        }
        break;

    case MSG_STA_CONNECTED:
        s_link_up = true;
        s_connect_pending = false;
        s_no_ip_since_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        memcpy(s_status.bssid, m->bssid, 6);
        s_status.channel = m->channel;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Verbunden mit " MACSTR " auf Kanal %d", MAC2STR(m->bssid), m->channel);
        break;

    case MSG_STA_DISCONNECTED: {
        bool was_up = s_link_up;
        s_link_up = false;
        s_connect_pending = false;
        s_no_ip_since_us = 0;
        portENTER_CRITICAL(&s_lock);
        s_status.last_reason = m->reason;
        s_status.ip[0] = '\0';
        s_status.rssi = 0;
        if (was_up && !s_roaming) s_status.disconnects++;
        portEXIT_CRITICAL(&s_lock);

        if (s_roaming) {
            s_roaming = false;
            portENTER_CRITICAL(&s_lock);
            s_status.roams++;
            portEXIT_CRITICAL(&s_lock);
            start_connect();             // sofort zum gewählten AP
        } else {
            ESP_LOGW(TAG, "Getrennt, Grund %u", m->reason);
            clear_bssid_lock();
            schedule_retry();
        }
        break;
    }

    case MSG_GOT_IP: {
        esp_ip4_addr_t ip = { .addr = m->ip };
        portENTER_CRITICAL(&s_lock);
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ip));
        s_status.state = NET_WIFI_CONNECTED;
        portEXIT_CRITICAL(&s_lock);
        s_backoff_ms = BACKOFF_MIN_MS;
        s_no_ip_since_us = 0;
        s_last_roam_us = esp_timer_get_time();
        update_rssi();
        ESP_LOGI(TAG, "IP " IPSTR, IP2STR(&ip));
        {
            static bool tested;
            if (!tested) {
                tested = true;
                xTaskCreate(net_selftest_task, "net_test", 4096, NULL, 3, NULL);
            }
        }
        break;
    }

    case MSG_LOST_IP:
        portENTER_CRITICAL(&s_lock);
        s_status.ip[0] = '\0';
        if (s_status.state == NET_WIFI_CONNECTED) s_status.state = NET_WIFI_CONNECTING;
        portEXIT_CRITICAL(&s_lock);
        if (s_link_up && s_no_ip_since_us == 0) {
            s_no_ip_since_us = esp_timer_get_time();
        }
        ESP_LOGW(TAG, "IP verloren");
        break;

    case MSG_SCAN_DONE:
        if (s_ignore_scan_done) {
            s_ignore_scan_done = false;
            break;
        }
        roam_evaluate();
        break;
    }
}

/*
 * Watchdog für die SDIO-Verbindung zum C6. Hängt der C6 (RPC-Timeouts), kommen keine
 * WLAN-Ereignisse mehr und die Anzeige bliebe auf "verbunden" stehen. Letzter Ausweg ist
 * ein Neustart des P4; dabei wird der C6 über C6_EN ebenfalls zurückgesetzt.
 */
static void health_check(void)
{
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        if (s_health_fails > 0) {
            ESP_LOGI(TAG, "C6 antwortet wieder");
        }
        s_health_fails = 0;
        return;
    }
    s_health_fails++;
    ESP_LOGE(TAG, "C6 antwortet nicht (%d/%d)", s_health_fails, HEALTH_MAX_FAILS);
    portENTER_CRITICAL(&s_lock);
    s_status.state = NET_WIFI_DISCONNECTED;
    s_status.ip[0] = '\0';
    s_status.rssi = 0;
    portEXIT_CRITICAL(&s_lock);
    if (s_health_fails >= HEALTH_MAX_FAILS) {
        ESP_LOGE(TAG, "Verbindung zum C6 verloren - Neustart");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

static void periodic(void)
{
    const int64_t now = esp_timer_get_time();

    if (s_sta_started && now - s_last_health_us > (int64_t)HEALTH_CHECK_MS * 1000) {
        s_last_health_us = now;
        health_check();
        if (s_health_fails > 0) {
            return;     // solange der C6 hängt, keine weiteren RPC-Aufrufe
        }
    }
    net_wifi_state_t st;
    portENTER_CRITICAL(&s_lock);
    st = s_status.state;
    portEXIT_CRITICAL(&s_lock);

    if (st == NET_WIFI_DISCONNECTED && !s_connect_pending && now >= s_next_try_us) {
        start_connect();
        return;
    }
    /* Verbindungsversuch hängt (kein CONNECTED/DISCONNECTED-Event) */
    if (s_connect_pending && now - s_connect_started_us > (int64_t)CONNECT_TIMEOUT_MS * 1000) {
        ESP_LOGW(TAG, "Verbindungsversuch hängt, neu starten");
        s_connect_pending = false;
        esp_wifi_disconnect();
        clear_bssid_lock();
        schedule_retry();
        return;
    }
    /* Mit AP verbunden, aber keine IP (DHCP hängt, Mesh-Knoten noch ohne Backhaul) */
    if (!s_static_ip && s_link_up && s_no_ip_since_us != 0 && now - s_no_ip_since_us > (int64_t)DHCP_TIMEOUT_MS * 1000) {
        ESP_LOGW(TAG, "Keine IP nach %d s - neu verbinden", DHCP_TIMEOUT_MS / 1000);
        s_no_ip_since_us = 0;
        portENTER_CRITICAL(&s_lock);
        memcpy(s_bad_bssid, s_status.bssid, 6);
        portEXIT_CRITICAL(&s_lock);
        s_bad_until_us = now + (int64_t)BAD_AP_HOLD_MS * 1000;
        esp_wifi_disconnect();      // -> DISCONNECTED-Event -> Backoff -> neuer Versuch
        return;
    }
    if (st == NET_WIFI_CONNECTED) {
        if (now - s_last_rssi_us > (int64_t)RSSI_POLL_MS * 1000) {
            s_last_rssi_us = now;
            update_rssi();
        }
        if (now - s_last_roam_us > (int64_t)ROAM_CHECK_MS * 1000) {
            s_last_roam_us = now;
            roam_check();
        }
    }
}

static void wifi_task(void *arg)
{
    msg_t m;
    for (;;) {
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(TASK_TICK_MS)) == pdTRUE) {
            handle_msg(&m);
        }
        periodic();
    }
}

/* ------------------------------------------------------------------ */

const char *net_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

int net_wifi_scan(net_wifi_ap_t *out, int max)
{
    if (s_scanning) return -1;
    wifi_scan_config_t sc = { .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    s_ignore_scan_done = true;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        s_ignore_scan_done = false;
        return -1;
    }
    uint16_t n = 32;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    int count = 0;
    if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (int i = 0; i < n && count < max; i++) {
            if (!recs[i].ssid[0]) continue;
            bool dup = false;                       // Mesh: gleiche SSID nur einmal (stärkste zuerst)
            for (int k = 0; k < count; k++) dup |= !strcmp(out[k].ssid, (const char *)recs[i].ssid);
            if (dup) continue;
            strlcpy(out[count].ssid, (const char *)recs[i].ssid, sizeof(out[count].ssid));
            out[count].rssi = recs[i].rssi;
            out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
            count++;
        }
    }
    free(recs);
    return count;
}

esp_err_t net_wifi_start(const net_wifi_cfg_t *c)
{
    ESP_RETURN_ON_FALSE(s_queue == NULL, ESP_ERR_INVALID_STATE, TAG, "schon gestartet");
    const char *ssid = c->ssid;
    const char *password = c->password;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.hostname, sizeof(s_status.hostname), "loxpanel-%02x%02x", mac[4], mac[5]);
    portEXIT_CRITICAL(&s_lock);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "LoxPanel-%02X%02X", mac[4], mac[5]);

    s_configured = !c->setup_ap && ssid != NULL && ssid[0] != '\0';
    if (!s_configured) {
        ssid = "";
        ESP_LOGW(TAG, "Keine SSID eingerichtet – Einrichtungsmodus");
    }

    s_queue = xQueueCreate(16, sizeof(msg_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "Queue");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");

    s_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif sta");
    esp_netif_set_hostname(s_netif, s_status.hostname);

    if (!s_configured) {
        s_ap_netif = esp_netif_create_default_wifi_ap();    // 192.168.4.1 mit DHCP-Server
        ESP_RETURN_ON_FALSE(s_ap_netif, ESP_FAIL, TAG, "netif ap");
    }

    if (s_configured && c->ip && c->ip[0]) {
        esp_netif_ip_info_t ip = { 0 };
        if (esp_netif_str_to_ip4(c->ip, &ip.ip) == ESP_OK &&
            esp_netif_str_to_ip4(c->mask && c->mask[0] ? c->mask : "255.255.255.0", &ip.netmask) == ESP_OK &&
            esp_netif_str_to_ip4(c->gw ? c->gw : "", &ip.gw) == ESP_OK) {
            esp_netif_dhcpc_stop(s_netif);
            esp_netif_set_ip_info(s_netif, &ip);
            esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4 = ip.gw };
            esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
            s_static_ip = true;
            ESP_LOGI(TAG, "Feste IP %s", c->ip);
        } else {
            ESP_LOGE(TAG, "Feste IP ungültig, nutze DHCP");
        }
    }

    set_state(NET_WIFI_STARTING);
    ESP_LOGI(TAG, "Starte Verbindung zum ESP32-C6");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL), TAG, "evt");

    memset(&s_cfg, 0, sizeof(s_cfg));
    strlcpy((char *)s_cfg.sta.ssid, ssid, sizeof(s_cfg.sta.ssid));
    if (password != NULL) {
        strlcpy((char *)s_cfg.sta.password, password, sizeof(s_cfg.sta.password));
    }
    s_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;          // alle APs des Mesh sehen ...
    s_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;      // ... und den stärksten nehmen
    s_cfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    s_cfg.sta.pmf_cfg.capable = true;
    s_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    s_cfg.sta.rm_enabled = 1;                               // 802.11k
    s_cfg.sta.btm_enabled = 1;                              // 802.11v (Steering durch Mesh)
    s_cfg.sta.failure_retry_cnt = 1;

    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
    portEXIT_CRITICAL(&s_lock);

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(s_configured ? WIFI_MODE_STA : WIFI_MODE_APSTA), TAG, "mode");
    if (c->mac && c->mac[0]) {
        unsigned m[6];
        if (sscanf(c->mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            uint8_t mac_new[6];
            for (int i = 0; i < 6; i++) mac_new[i] = (uint8_t)m[i];
            esp_err_t me = esp_wifi_set_mac(WIFI_IF_STA, mac_new);
            ESP_LOGI(TAG, "Eigene WLAN-MAC " MACSTR ": %s", MAC2STR(mac_new), esp_err_to_name(me));
        } else {
            ESP_LOGE(TAG, "WLAN-MAC ungültig: %s", c->mac);
        }
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &s_cfg), TAG, "config");
    if (!s_configured) {
        wifi_config_t ap = { 0 };
        strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = strlen(s_ap_ssid);
        ap.ap.channel = 6;
        ap.ap.max_connection = 4;
        ap.ap.authmode = WIFI_AUTH_OPEN;        // Einrichtung: offen, nur bis WLAN eingerichtet ist
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
        set_state(NET_WIFI_SETUP_AP);
        ESP_LOGI(TAG, "Einrichtungs-Access-Point \"%s\" auf 192.168.4.1", s_ap_ssid);
    }
    esp_wifi_set_ps(WIFI_PS_NONE);   // Netzbetrieb: geringe Latenz wichtiger als Strom

    BaseType_t ok = xTaskCreate(wifi_task, "net_wifi", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Task");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    ESP_LOGI(TAG, "WLAN gestartet, Hostname %s, SSID \"%s\"", s_status.hostname, ssid);
    return ESP_OK;
}

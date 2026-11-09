#pragma once

/*
 * WLAN über den ESP32-C6 (esp_hosted).
 * Hält die Verbindung selbstständig: Reconnect mit Backoff, Wechsel auf stärkeren
 * Access Point derselben SSID (Mesh-Roaming), ohne Neustart.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_WIFI_UNCONFIGURED,  // keine SSID gesetzt
    NET_WIFI_STARTING,      // Verbindung zum C6 wird aufgebaut
    NET_WIFI_CONNECTING,    // verbindet mit Access Point
    NET_WIFI_CONNECTED,     // verbunden und IP erhalten
    NET_WIFI_DISCONNECTED,  // getrennt, nächster Versuch geplant
    NET_WIFI_SETUP_AP,      // Einrichtungs-Access-Point aktiv
} net_wifi_state_t;

typedef struct {
    const char *ssid;
    const char *password;
    const char *ip;         // leer/NULL = DHCP
    const char *mask;
    const char *gw;
    const char *mac;        // leer/NULL = MAC des C6
    bool setup_ap;          // Access Point "LoxPanel-XXXX" statt Verbindung
} net_wifi_cfg_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} net_wifi_ap_t;

typedef struct {
    net_wifi_state_t state;
    char ssid[33];
    char ip[16];
    char hostname[32];
    char c6_fw[24];         // Firmware-Version des ESP32-C6
    int8_t rssi;            // dBm, 0 = unbekannt
    uint8_t channel;
    uint8_t bssid[6];
    uint8_t last_reason;    // letzter Trennungsgrund (wifi_err_reason_t)
    uint32_t disconnects;   // Trennungen seit Start
    uint32_t roams;         // Wechsel auf anderen Access Point
    uint32_t retry_in_ms;   // Wartezeit bis zum nächsten Versuch (wenn getrennt)
} net_wifi_status_t;

/* Startet WLAN. Erwartet initialisiertes NVS. Ohne SSID oder mit setup_ap -> Access Point. */
esp_err_t net_wifi_start(const net_wifi_cfg_t *cfg);

/* SSID des Einrichtungs-Access-Points ("LoxPanel-XXXX") */
const char *net_wifi_ap_ssid(void);

/* WLANs in der Umgebung (blockiert ca. 2-3 s). Rückgabe Anzahl oder -1. */
int net_wifi_scan(net_wifi_ap_t *out, int max);

void net_wifi_get_status(net_wifi_status_t *out);
bool net_wifi_is_connected(void);
const char *net_wifi_state_text(net_wifi_state_t state);

#ifdef __cplusplus
}
#endif

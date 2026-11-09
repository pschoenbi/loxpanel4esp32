#pragma once

/*
 * Geräte-Einstellungen (SPEC 5: getrennt von der Display-Konfiguration, werden nie exportiert).
 * Liegen im NVS (Namespace "dev"). Beim allerersten Start werden sie einmalig aus den
 * Kconfig-Werten (sdkconfig) übernommen, danach nur noch über Weboberfläche/Einrichtung.
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char ip[16];            // leer = DHCP
    char mask[16];
    char gw[16];
    char mac[18];           // leer = MAC des C6
    char ms_host[64];
    char ms_user[64];
    char ms_pass[96];       // wird nach erhaltenem Token gelöscht
    char web_hash[65];      // SHA256 (hex) des Web-Passworts, leer = noch nicht gesetzt
} dev_settings_t;

esp_err_t settings_init(void);

/* Kopie der aktuellen Einstellungen */
void settings_get(dev_settings_t *out);

/* Speichern (komplett) */
esp_err_t settings_save(const dev_settings_t *s);

/* WLAN eingerichtet? */
bool settings_has_wifi(void);

/* Web-Passwort setzen / prüfen */
esp_err_t settings_set_web_password(const char *pw);
bool settings_check_web_password(const char *pw);
bool settings_has_web_password(void);

/* Miniserver-Passwort löschen (nach erhaltenem Token, SPEC 4.2) */
void settings_forget_ms_password(void);

/* Einrichtungsmodus beim nächsten Start anfordern / Anforderung abholen (löscht sie) */
void settings_request_setup(void);
bool settings_take_setup_request(void);

#ifdef __cplusplus
}
#endif

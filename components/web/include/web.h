#pragma once

/*
 * Weboberfläche (SPEC 6).
 *   Einrichtungsmodus: Access Point, Captive Portal (DNS antwortet immer 192.168.4.1),
 *                      Formular für WLAN, Miniserver, Web-Passwort. Ohne Anmeldung.
 *   LAN-Modus:         http://loxpanel-xxxx.local, Basic Auth (Benutzer "admin"),
 *                      Status, Einstellungen, Export/Import von panel.json, Neustart.
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_start(bool setup_mode);

#ifdef __cplusplus
}
#endif

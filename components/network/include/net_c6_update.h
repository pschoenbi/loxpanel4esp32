#pragma once

/*
 * Einmaliges Update der ESP32-C6-Firmware über die esp_hosted-Verbindung.
 * Nur vorhanden mit CONFIG_LOXPANEL_C6_UPDATE=y.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_C6_UPDATE_NOT_NEEDED,   // C6 hat bereits die eingebettete Version
    NET_C6_UPDATE_DONE,         // übertragen, Host muss neu starten
    NET_C6_UPDATE_FAILED,       // Fehler, C6 läuft weiter mit alter Firmware
} net_c6_update_result_t;

/* Fortschritt: Text und Prozent (0–100, -1 = unbekannt) */
typedef void (*net_c6_progress_cb_t)(const char *text, int percent);

/* Vor net_wifi_start() aufrufen. Blockiert bis zum Ende (ca. 1–2 Minuten). */
net_c6_update_result_t net_c6_update_run(net_c6_progress_cb_t cb);

#ifdef __cplusplus
}
#endif

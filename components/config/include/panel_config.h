#pragma once

/*
 * Display-Konfiguration (/config/panel.json, SPEC Abschnitt 5).
 *
 *   {
 *     "schema": 1, "vorlage": "...", "vorlage_version": 1,
 *     "variablen": { "raum": "Wohnzimmer" },
 *     "anzeige": { "helligkeit": 80, "dimmen_nach_s": 60, "startseite": 0 },
 *     "seiten": [
 *       { "titel": "Licht", "raster": [5, 3],
 *         "kacheln": [ { "typ": "auto", "ziel": "{raum}/Licht Decke", "breite": 2, "hoehe": 1, "titel": "..." } ] },
 *       { "titel": "Wohnen", "auto": { "raum": "{raum}" } },        // alle Bausteine eines Raums
 *       { "titel": "Favoriten", "auto": { "favoriten": true } }      // Favoriten aus Loxone Config
 *     ],
 *     "abweichungen": { "Licht/2": { "ausblenden": true }, "Licht/3": { "ziel": "Küche/Spots" } }
 *   }
 *
 * Schlüssel in "abweichungen": "<Seitentitel>/<Kachelnummer ab 1>".
 *
 * "typ" schränkt die Suche bei "Raum/Name" ein (gleiche Namen sind in Loxone häufig):
 *   auto (alle), schalter, dimmer, jalousie, licht, raumregler, zaehler, energie, tuer (Tür & Zutritt), kamera, info
 * Seitentitel werden ebenfalls mit Variablen ersetzt; Schlüssel nutzen den ersetzten Titel.
 */

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_SCHEMA          1
#define CFG_PATH            "/config/panel.json"

typedef enum {
    CFG_BIND_PENDING,       // Struktur noch nicht geladen
    CFG_BIND_OK,
    CFG_BIND_MISSING,       // nicht gefunden -> "nicht zugeordnet"
    CFG_BIND_AMBIGUOUS,     // mehrere Treffer -> Auswahl anbieten
} cfg_bind_t;

typedef struct {
    char key[72];           // "<Seite>/<Nr>" für abweichungen
    char typ[16];           // "auto", "schalter", "dimmer", "jalousie"
    char ziel[112];         // nach Ersetzen der Platzhalter
    char titel[48];         // optionaler Anzeigename
    uint8_t breite, hoehe;
    bool from_auto;         // stammt von einer Auto-Seite (Baustein nicht änderbar, nur ausblenden)
    /* Ergebnis der Auflösung */
    cfg_bind_t bind;
    char uuid[48];          // uuidAction des Bausteins
} cfg_tile_t;

typedef struct {
    char titel[48];
    uint8_t cols, rows;     // 0 = aus Auflösung
    bool is_auto;
    char auto_raum[64];
    bool auto_favoriten;
    cfg_tile_t *tiles;
    int count;
} cfg_page_t;

typedef struct {
    int schema;
    char vorlage[48];
    int vorlage_version;
    int helligkeit;
    int dimmen_nach_s;
    int startseite;
    char raum[64];          // Variable {raum}, für die Kopfzeile
    cfg_page_t *pages;
    int page_count;
} panel_cfg_t;

/* Dateisystem einhängen, Vorlage ggf. übernehmen, Konfiguration laden. */
esp_err_t cfg_init(void);

/* Aktuelle Konfiguration (nur im UI-Task benutzen). */
const panel_cfg_t *cfg_get(void);

/* Steigt bei jeder Änderung (neu geladen, Abweichung gesetzt, neu aufgelöst). */
uint32_t cfg_generation(void);

/* Ziele gegen die Loxone-Struktur auflösen und Auto-Seiten füllen.
   Aufruf unter lox_struct_lock. */
void cfg_resolve(void);

/* Abweichung setzen und speichern: ziel (NULL = unverändert), ausblenden. */
esp_err_t cfg_set_override(const char *key, const char *ziel, bool hide);

/* Variable lesen ("raum" ...), NULL falls nicht gesetzt. */
const char *cfg_var(const char *name);

/* ---- Für die Weboberfläche (aus beliebigem Task) ---- */

/* Aktuelle Datei als Text (heap_caps_free), NULL bei Fehler. */
char *cfg_read_text(size_t *len);

/* Prüfen (Schema, Aufbau), alte Datei als Backup sichern, neue schreiben.
   Übernommen wird sie im UI-Task (cfg_poll). err: Klartext bei Fehler. */
esp_err_t cfg_import(const char *text, size_t len, char *err, size_t err_size);

/* Backup zurückspielen (eine Stufe). */
esp_err_t cfg_undo(void);
bool cfg_has_backup(void);

/* Im UI-Task regelmässig aufrufen: lädt nach Import/Undo neu. */
void cfg_poll(void);

#ifdef __cplusplus
}
#endif

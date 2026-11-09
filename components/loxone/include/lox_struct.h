#pragma once

/*
 * Loxone-Strukturdatei (LoxAPP3.json) im PSRAM.
 * Liefert Räume, Kategorien, Bausteine und eine schnelle Zuordnung Status-UUID -> Baustein.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UUID in kanonischer Byte-Reihenfolge (wie im Text "d1-d2-d3-d4", big endian) */
typedef struct {
    uint8_t b[16];
} lox_uuid_t;

typedef struct {
    lox_uuid_t uuid;
    const char *control_name;   // Name des Bausteins
    const char *control_type;   // Loxone-Typ, z. B. "Switch"
    const char *state_name;     // Name des Status, z. B. "active"
    const char *room_name;      // Raum (kann NULL sein)
} lox_state_ref_t;

typedef struct {
    char ms_name[48];
    char last_modified[32];
    size_t rooms;
    size_t categories;
    size_t controls;
    size_t states;
} lox_struct_info_t;

/* Übernimmt den Text (muss mit heap_caps_malloc in PSRAM liegen, wird freigegeben). */
esp_err_t lox_struct_load(char *json, size_t len);
void lox_struct_free(void);
bool lox_struct_loaded(void);
void lox_struct_get_info(lox_struct_info_t *out);

/* Status-UUID nachschlagen, NULL falls unbekannt. */
const lox_state_ref_t *lox_struct_find_state(const lox_uuid_t *uuid);

/* Wurzel des JSON-Baums (nur lesen). Zugriff nur zwischen lox_struct_lock/unlock,
   da die Struktur beim Neuladen ausgetauscht wird. */
const cJSON *lox_struct_root(void);
bool lox_struct_lock(uint32_t timeout_ms);
/* Raumname eines Bausteins (NULL falls keiner); nur unter lox_struct_lock. */
const char *lox_struct_room_of(const cJSON *ctrl);
/* Baustein (auch Unterbaustein) per uuidAction; room = Raum (geerbt vom Elternbaustein). */
const cJSON *lox_struct_find_control(const char *uuid_action, const char **room);
void lox_struct_unlock(void);

/* Zähler, der bei jedem Neuladen der Struktur steigt (UI muss dann neu aufbauen). */
uint32_t lox_struct_generation(void);

/* Aktuelle Werte der Status (vom Client geschrieben, von der UI gelesen; threadsicher). */
bool lox_value_get(const lox_uuid_t *uuid, double *out);
bool lox_value_get_str(const char *uuid, double *out);
bool lox_text_get(const lox_uuid_t *uuid, char *out, size_t size);
/* Steigt bei jeder Wertänderung: billiger Test, ob die UI etwas aktualisieren muss. */
uint32_t lox_value_changes(void);

/* Zeitplan (Daytimer) eines Status "entriesAndDefaultValue" */
typedef struct {
    int mode;           // Betriebsart (Wochentag/Feiertag ...), siehe Daytimer-Status "mode"
    int from_min;       // Minuten seit Mitternacht
    int to_min;
    int need_activate;
    double value;
} lox_dt_entry_t;

/* Liefert Anzahl Einträge (max. max) und Standardwert, -1 wenn unbekannt. */
int lox_daytimer_get(const lox_uuid_t *uuid, double *def_value, lox_dt_entry_t *out, int max);

/* Nur für den Client */
void lox_value_set(const lox_uuid_t *uuid, double v);
void lox_text_set(const lox_uuid_t *uuid, const char *text);
void lox_daytimer_set(const lox_uuid_t *uuid, double def_value, const lox_dt_entry_t *e, int n);

bool lox_uuid_parse(const char *s, lox_uuid_t *out);
void lox_uuid_format(const lox_uuid_t *u, char *out /* >= 37 */);
/* Binäre UUID aus Statusmeldung (little endian Felder) -> kanonisch */
void lox_uuid_from_wire(const uint8_t *p, lox_uuid_t *out);

#ifdef __cplusplus
}
#endif

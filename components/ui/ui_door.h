#pragma once

/*
 * Tür & Zutritt: Torsteuerung (Gate), NFC Code Touch, Fenster-/Türüberwachung (WindowMonitor).
 * Detailansichten (Popup). Die Kacheln baut ui_ctile.c, Texte kommen von hier.
 */

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "lvgl.h"
#include "lox_struct.h"

typedef struct ui_door ui_door_t;

bool ui_door_supports(const char *type);

/* Detailansicht in einen Container bauen (unter lox_struct_lock). */
ui_door_t *ui_door_detail_create(lv_obj_t *parent, const cJSON *ctrl, const char *room);
void ui_door_update(ui_door_t *d);
void ui_door_free(ui_door_t *d);

/* Kacheltexte */
void ui_gate_text(double pos, double active, char *out, size_t size);
/* Fensterüberwachung: Zusammenfassung aus numOpen/numTilted/numUnlocked/numOffline */
bool ui_winmon_text(double open, double tilted, double unlocked, double offline, char *out, size_t size);

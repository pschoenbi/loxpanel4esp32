#pragma once

/*
 * Kompakte Kachel für das Seitenraster. Passt sich der Grösse an (breite x hoehe in Zellen).
 *   Schalter: Tippen schaltet      Taster: Tippen = Impuls
 *   Dimmer:   Tippen ein/aus, ab 2 Zellen breit mit Schieberegler, lang drücken = Detail
 *   Jalousie: Pfeile (Tippen ganz, Halten Schritt), lang drücken = Detail
 *   Info/Zähler/Text: Anzeige
 *   Nicht zugeordnet / mehrdeutig: Tippen öffnet die Auswahl aus der Struktur
 */

#include "cJSON.h"
#include "lvgl.h"
#include "panel_config.h"

typedef struct ui_ctile ui_ctile_t;

/* Unter lox_struct_lock aufrufen. ctrl = NULL, wenn nicht aufgelöst. */
ui_ctile_t *ui_ctile_create(lv_obj_t *parent, const cfg_tile_t *ct, const cJSON *ctrl, const char *room,
                            int w, int h);
void ui_ctile_update(ui_ctile_t *t);
/* Gibt nur die Verwaltungsdaten frei; die LVGL-Objekte gehören dem Elternobjekt. */
void ui_ctile_free(ui_ctile_t *t);

/* Popups (Detail eines Bausteins, Auswahl für eine Kachel) */
/* ct darf NULL sein (dann ohne Ausblenden/Ändern); value_text = aktueller Wert für reine Anzeigen */
void ui_popup_control(const char *uuid_action, const cfg_tile_t *ct, const char *value_text);
void ui_popup_picker(const cfg_tile_t *ct);
void ui_popup_update(void);
void ui_popup_close(void);

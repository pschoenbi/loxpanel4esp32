#pragma once

/*
 * Energieflussmonitor (EFM): Kachel und Detailansicht mit Flussdiagramm,
 * aufgebaut wie in der Loxone-App.
 */

#include "cJSON.h"
#include "lvgl.h"

typedef struct ui_efm ui_efm_t;

/* Kachelinhalt in einen vorhandenen Kachel-Container bauen (unter lox_struct_lock). */
ui_efm_t *ui_efm_tile_create(lv_obj_t *tile, const cJSON *ctrl, const char *title, const char *room, int w, int h);

/* Detailansicht in einen Container bauen (unter lox_struct_lock). */
ui_efm_t *ui_efm_detail_create(lv_obj_t *parent, const cJSON *ctrl, const char *room);

void ui_efm_update(ui_efm_t *f);
/* Gibt die Verwaltungsdaten frei und stoppt Animationen; LVGL-Objekte gehören dem Eltern-Container. */
void ui_efm_free(ui_efm_t *f);

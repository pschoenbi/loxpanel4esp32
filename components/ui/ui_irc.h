#pragma once

/*
 * Intelligente Raumregelung (IRoomControllerV2): Kachel, Detailansicht, Einstellungen.
 * Aufbau orientiert sich an der Loxone-App.
 */

#include "cJSON.h"
#include "lvgl.h"

typedef struct ui_irc ui_irc_t;

/* Kachelinhalt in einen vorhandenen Kachel-Container bauen (unter lox_struct_lock). */
ui_irc_t *ui_irc_tile_create(lv_obj_t *tile, const cJSON *ctrl, const char *title, int w, int h);

/* Detailansicht (inkl. Einstellungen) in einen Container bauen (unter lox_struct_lock). */
ui_irc_t *ui_irc_detail_create(lv_obj_t *parent, const cJSON *ctrl, const char *room);

void ui_irc_update(ui_irc_t *r);
/* Gibt nur die Verwaltungsdaten frei; LVGL-Objekte gehören dem Eltern-Container. */
void ui_irc_free(ui_irc_t *r);

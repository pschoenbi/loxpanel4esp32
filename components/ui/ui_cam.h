#pragma once

/*
 * Türsprechstelle / Kamera (Loxone "Intercom" und "IntercomV2"): Livebild + Ausgänge.
 * Video-URL und Zugangsdaten kommen aus den gesicherten Details (lox_secured_details).
 */

#include "cJSON.h"
#include "lvgl.h"

typedef struct ui_cam ui_cam_t;

/* Detailansicht in einen Container bauen (unter lox_struct_lock). */
ui_cam_t *ui_cam_detail_create(lv_obj_t *parent, const cJSON *ctrl, const char *room);
void ui_cam_update(ui_cam_t *c);
/* Stoppt den Stream; LVGL-Objekte gehören dem Eltern-Container. */
void ui_cam_free(ui_cam_t *c);

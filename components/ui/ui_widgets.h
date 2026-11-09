#pragma once

/*
 * Widgets je Loxone-Bausteintyp. Ein Widget kopiert beim Erzeugen alle nötigen UUIDs
 * und braucht die Strukturdatei danach nicht mehr.
 */

#include <stdbool.h>
#include "cJSON.h"
#include "lvgl.h"

typedef struct ui_widget ui_widget_t;

/* Unterstützt der Typ ein Widget? ("Switch", "Pushbutton", "Dimmer", "EIBDimmer", "Jalousie") */
bool ui_widget_supports(const char *loxone_type);

/* Unter lox_struct_lock aufrufen. room darf NULL sein. */
ui_widget_t *ui_widget_create(lv_obj_t *parent, const cJSON *ctrl, const char *room);
void ui_widget_update(ui_widget_t *w);
/* Löscht die LVGL-Objekte und gibt das Widget frei. */
void ui_widget_destroy(ui_widget_t *w);

/* Raumregler: activeMode als Text (0 Eco, 1 Komfort, 2 Gebäudeschutz, 3 Manuell, 4 Aus) */
const char *ui_irc_mode_text(int mode);

/* Lichtsteuerung: Stimmung 778 = "Aus" */
#include "lox_struct.h"
#define UI_MOOD_OFF 778
int ui_light_active_moods(const lox_uuid_t *active_state, int *ids, int max);
bool ui_light_is_off(const int *ids, int n);
void ui_light_mood_text(const lox_uuid_t *list_state, const int *ids, int n, char *out, size_t size);

#pragma once

#include "lvgl.h"

/* Farben */
#define UI_COL_BG       0x14171c
#define UI_COL_TILE     0x232a33
#define UI_COL_BTN      0x353d48
#define UI_COL_TEXT     0xe8eaed
#define UI_COL_DIM      0x9aa3ad
#define UI_COL_ON       0x69c350    // aktiv / verbunden
#define UI_COL_WARN     0xffa657
#define UI_COL_ERR      0xff6b6b

#define UI_COL_HEAT     0xf28c28    // Heizen (orange)
#define UI_COL_COOL     0x4aa3df    // Kühlen (blau)

typedef enum { UI_F_S, UI_F_M, UI_F_L, UI_F_XL, UI_F_XXL, UI_F_COUNT } ui_font_id_t;

/* Gezeichnete Symbole */
lv_obj_t *ui_icon_flame(lv_obj_t *parent, int size, lv_color_t c);
lv_obj_t *ui_icon_snow(lv_obj_t *parent, int size, lv_color_t c);
lv_obj_t *ui_icon_sun(lv_obj_t *parent, int size, lv_color_t c);
lv_obj_t *ui_icon_pylon(lv_obj_t *parent, int size, lv_color_t c);
lv_obj_t *ui_icon_car(lv_obj_t *parent, int size, lv_color_t c);

/* Wert in Pixel, skaliert von 600 px Displayhöhe auf die echte Höhe */
int ui_px(int v);
const lv_font_t *ui_font(ui_font_id_t id);

/* Kachel: abgerundeter Container im Kachelstil */
lv_obj_t *ui_tile_create(lv_obj_t *parent);

void ui_pages_init(lv_obj_t *parent, int top, int height, void (*on_page)(const char *title));
void ui_pages_update(void);
void ui_diag_create(lv_obj_t *page);
void ui_diag_update(void);

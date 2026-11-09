/*
 * Seiten aus der Konfiguration: je Seite ein Raster (spalten x zeilen), Kacheln belegen
 * breite x hoehe Zellen und werden der Reihe nach auf freie Plätze gesetzt.
 * Passen nicht alle Kacheln, wächst die Seite nach unten und lässt sich scrollen.
 * Als letzte Seite folgt immer die Diagnose.
 */

#include "ui_ctile.h"
#include "ui_priv.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lox_struct.h"
#include "panel_config.h"

static const char *TAG = "ui_pages";

#define MAX_ROWS    32

static lv_obj_t *s_parent;
static int s_top, s_height;
static lv_obj_t *s_tv;
static lv_obj_t *s_dots;
static int s_page_count;            // inkl. Diagnose
static ui_ctile_t **s_tiles;
static int s_tile_count, s_tile_cap;
static int32_t **s_dsc;             // Raster-Beschreibungen (LVGL behält die Zeiger)
static int s_dsc_count, s_dsc_cap;
static uint32_t s_cfg_gen, s_struct_gen, s_changes;
static bool s_built, s_built_resolved;
static void (*s_on_page)(const char *title);

static void track_dsc(int32_t *d)
{
    if (s_dsc_count == s_dsc_cap) {
        s_dsc_cap = s_dsc_cap ? s_dsc_cap * 2 : 16;
        s_dsc = heap_caps_realloc(s_dsc, s_dsc_cap * sizeof(*s_dsc), MALLOC_CAP_SPIRAM);
    }
    s_dsc[s_dsc_count++] = d;
}

static void track_tile(ui_ctile_t *t)
{
    if (t == NULL) return;
    if (s_tile_count == s_tile_cap) {
        s_tile_cap = s_tile_cap ? s_tile_cap * 2 : 32;
        s_tiles = heap_caps_realloc(s_tiles, s_tile_cap * sizeof(*s_tiles), MALLOC_CAP_SPIRAM);
    }
    s_tiles[s_tile_count++] = t;
}

static void clear_all(void)
{
    ui_popup_close();
    if (s_tv) lv_obj_delete(s_tv);
    if (s_dots) lv_obj_delete(s_dots);
    s_tv = s_dots = NULL;
    for (int i = 0; i < s_tile_count; i++) ui_ctile_free(s_tiles[i]);
    s_tile_count = 0;
    for (int i = 0; i < s_dsc_count; i++) heap_caps_free(s_dsc[i]);
    s_dsc_count = 0;
}

static const char *page_title(int idx)
{
    const panel_cfg_t *c = cfg_get();
    return idx < c->page_count ? c->pages[idx].titel : "Diagnose";
}

static void tv_changed(lv_event_t *e)
{
    lv_obj_t *tile = lv_tileview_get_tile_active(s_tv);
    int idx = lv_obj_get_index(tile);
    for (int i = 0; i < (int)lv_obj_get_child_count(s_dots); i++) {
        lv_obj_set_style_bg_opa(lv_obj_get_child(s_dots, i), i == idx ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    if (s_on_page) s_on_page(page_title(idx));
}

/* Standard-Raster aus der Auflösung: 4x3 bei 800 px, 5x3 bei 1024 px Breite */
static void default_grid(int *cols, int *rows)
{
    int w = lv_display_get_horizontal_resolution(NULL);
    *cols = (w + 100) / 205;
    if (*cols < 2) *cols = 2;
    *rows = 3;
}

static void build_page(lv_obj_t *page, const cfg_page_t *p, bool resolved)
{
    int cols, rows;
    default_grid(&cols, &rows);
    if (p->cols > 0 && p->rows > 0) {
        cols = p->cols;
        rows = p->rows;
    }

    /* Plätze vergeben */
    static bool used[MAX_ROWS][16];
    memset(used, 0, sizeof(used));
    if (cols > 16) cols = 16;
    int max_row = rows;

    const int gap = ui_px(12);
    const int pad = ui_px(14);
    const int avail_h = s_height - 2 * pad - ui_px(20);     // unten Platz für die Punkte
    const int cell_h = (avail_h - (rows - 1) * gap) / rows;

    typedef struct { int c, r, w, h; } place_t;
    place_t *pl = heap_caps_calloc(p->count ? p->count : 1, sizeof(place_t), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < p->count; i++) {
        int w = p->tiles[i].breite > cols ? cols : p->tiles[i].breite;
        int h = p->tiles[i].hoehe;
        pl[i].w = w;
        pl[i].h = h;
        pl[i].c = -1;
        for (int r = 0; r + h <= MAX_ROWS && pl[i].c < 0; r++) {
            for (int c = 0; c + w <= cols; c++) {
                bool free_ = true;
                for (int y = r; y < r + h && free_; y++)
                    for (int x = c; x < c + w && free_; x++)
                        if (used[y][x]) free_ = false;
                if (free_) {
                    for (int y = r; y < r + h; y++)
                        for (int x = c; x < c + w; x++) used[y][x] = true;
                    pl[i].c = c;
                    pl[i].r = r;
                    if (r + h > max_row) max_row = r + h;
                    break;
                }
            }
        }
        if (pl[i].c < 0) ESP_LOGW(TAG, "Seite \"%s\": kein Platz für Kachel %d", p->titel, i + 1);
    }

    int32_t *cd = heap_caps_malloc((cols + 1) * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    int32_t *rd = heap_caps_malloc((max_row + 1) * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < cols; i++) cd[i] = LV_GRID_FR(1);
    cd[cols] = LV_GRID_TEMPLATE_LAST;
    for (int i = 0; i < max_row; i++) rd[i] = cell_h;
    rd[max_row] = LV_GRID_TEMPLATE_LAST;
    track_dsc(cd);
    track_dsc(rd);

    lv_obj_set_style_pad_all(page, pad, 0);
    lv_obj_set_style_pad_bottom(page, pad + ui_px(20), 0);
    lv_obj_set_style_pad_column(page, gap, 0);
    lv_obj_set_style_pad_row(page, gap, 0);
    lv_obj_set_grid_dsc_array(page, cd, rd);
    lv_obj_set_layout(page, LV_LAYOUT_GRID);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    if (p->count == 0) {
        lv_obj_t *l = lv_label_create(page);
        lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_DIM), 0);
        lv_label_set_text(l, !resolved ? "Warte auf Miniserver ..."
                             : p->is_auto ? "Keine Bausteine gefunden" : "Keine Kacheln");
        lv_obj_set_grid_cell(l, LV_GRID_ALIGN_START, 0, cols, LV_GRID_ALIGN_START, 0, 1);
    }

    for (int i = 0; i < p->count; i++) {
        if (pl[i].c < 0) continue;
        const cfg_tile_t *ct = &p->tiles[i];
        const cJSON *ctrl = NULL;
        const char *room = NULL;
        if (resolved && ct->bind == CFG_BIND_OK) ctrl = lox_struct_find_control(ct->uuid, &room);
        ui_ctile_t *t = ui_ctile_create(page, ct, ctrl, room, pl[i].w, pl[i].h);
        if (t) {
            lv_obj_t *o = lv_obj_get_child(page, -1);
            lv_obj_set_grid_cell(o, LV_GRID_ALIGN_STRETCH, pl[i].c, pl[i].w, LV_GRID_ALIGN_STRETCH, pl[i].r, pl[i].h);
            track_tile(t);
        }
    }
    heap_caps_free(pl);
}

static void build(bool resolved)
{
    int active = 0;
    if (s_tv) active = lv_obj_get_index(lv_tileview_get_tile_active(s_tv));
    else active = cfg_get()->startseite;
    clear_all();

    const panel_cfg_t *c = cfg_get();
    s_page_count = c->page_count + 1;
    if (active >= s_page_count) active = 0;

    s_tv = lv_tileview_create(s_parent);
    lv_obj_set_size(s_tv, LV_PCT(100), s_height);
    lv_obj_align(s_tv, LV_ALIGN_TOP_MID, 0, s_top);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tv, tv_changed, LV_EVENT_VALUE_CHANGED, NULL);

    for (int i = 0; i < s_page_count; i++) {
        lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < s_page_count - 1 ? LV_DIR_RIGHT : 0);
        lv_obj_t *page = lv_tileview_add_tile(s_tv, i, 0, dir);
        if (i < c->page_count) {
            build_page(page, &c->pages[i], resolved);
        } else {
            lv_obj_set_style_pad_all(page, ui_px(16), 0);
            lv_obj_set_style_pad_bottom(page, ui_px(30), 0);
            ui_diag_create(page);
        }
    }

    s_dots = lv_obj_create(s_parent);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_dots, ui_px(10), 0);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -ui_px(8));
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < s_page_count; i++) {
        lv_obj_t *d = lv_obj_create(s_dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, ui_px(11), ui_px(11));
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(UI_COL_TEXT), 0);
    }

    lv_tileview_set_tile_by_index(s_tv, active, 0, LV_ANIM_OFF);
    tv_changed(NULL);
    s_built = true;
    s_built_resolved = resolved;
    ESP_LOGI(TAG, "%d Seiten, %d Kacheln aufgebaut%s", s_page_count, s_tile_count,
             resolved ? "" : " (ohne Struktur)");
}

void ui_pages_init(lv_obj_t *parent, int top, int height, void (*on_page)(const char *title))
{
    s_parent = parent;
    s_top = top;
    s_height = height;
    s_on_page = on_page;
    build(false);
    s_cfg_gen = cfg_generation();
}

void ui_pages_update(void)
{
    bool loaded = lox_struct_loaded();
    bool need = !s_built || cfg_generation() != s_cfg_gen ||
                (loaded && (!s_built_resolved || lox_struct_generation() != s_struct_gen));
    if (need) {
        if (loaded) {
            if (!lox_struct_lock(100)) return;      // Struktur wird geladen: später
            uint32_t sg = lox_struct_generation();
            cfg_resolve();
            build(true);
            lox_struct_unlock();
            s_struct_gen = sg;
        } else {
            build(false);
        }
        s_cfg_gen = cfg_generation();
        s_changes = 0;
    }
    uint32_t ch = lox_value_changes();
    if (ch != s_changes) {
        s_changes = ch;
        for (int i = 0; i < s_tile_count; i++) ui_ctile_update(s_tiles[i]);
        ui_popup_update();
    }
}

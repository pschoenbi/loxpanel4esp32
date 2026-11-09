/*
 * Kleine gezeichnete Symbole (in den Schriften nicht enthalten): Flamme, Schneeflocke.
 */

#include "ui_priv.h"

#include <math.h>
#include "esp_heap_caps.h"

static void canvas_deleted(lv_event_t *e)
{
    lv_draw_buf_t *db = lv_event_get_user_data(e);
    if (db) {
        heap_caps_free(db->unaligned_data);
        heap_caps_free(db);
    }
}

/* Bildpuffer im PSRAM: der interne Speicher wird für WLAN (SDIO) gebraucht */
static lv_obj_t *canvas_new(lv_obj_t *parent, int s, lv_layer_t *layer)
{
    lv_obj_t *cv = lv_canvas_create(parent);
    uint32_t stride = lv_draw_buf_width_to_stride(s, LV_COLOR_FORMAT_ARGB8888);
    uint32_t size = stride * s;
    lv_draw_buf_t *db = heap_caps_calloc(1, sizeof(*db), MALLOC_CAP_SPIRAM);
    uint8_t *raw = heap_caps_malloc(size + LV_DRAW_BUF_ALIGN, MALLOC_CAP_SPIRAM);
    if (db == NULL || raw == NULL) {
        heap_caps_free(db);
        heap_caps_free(raw);
        return cv;      // leer statt Absturz
    }
    lv_draw_buf_init(db, s, s, LV_COLOR_FORMAT_ARGB8888, stride, lv_draw_buf_align(raw, LV_COLOR_FORMAT_ARGB8888), size);
    db->unaligned_data = raw;
    lv_canvas_set_draw_buf(cv, db);
    lv_obj_add_event_cb(cv, canvas_deleted, LV_EVENT_DELETE, db);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_canvas_init_layer(cv, layer);
    return cv;
}

static void drop(lv_layer_t *layer, float cx, float cy, float r, float tip_y, lv_color_t c)
{
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = c;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { (int32_t)(cx - r), (int32_t)(cy - r), (int32_t)(cx + r), (int32_t)(cy + r) };
    lv_draw_rect(layer, &rd, &a);

    lv_draw_triangle_dsc_t td;
    lv_draw_triangle_dsc_init(&td);
    td.color = c;
    td.p[0].x = cx;           td.p[0].y = tip_y;
    td.p[1].x = cx - r * 0.97f; td.p[1].y = cy - r * 0.25f;
    td.p[2].x = cx + r * 0.97f; td.p[2].y = cy - r * 0.25f;
    lv_draw_triangle(layer, &td);
}

lv_obj_t *ui_icon_flame(lv_obj_t *parent, int s, lv_color_t c)
{
    lv_layer_t layer;
    lv_obj_t *cv = canvas_new(parent, s, &layer);
    float r = s * 0.30f;
    drop(&layer, s / 2.0f, s * 0.64f, r, s * 0.04f, c);
    drop(&layer, s / 2.0f, s * 0.74f, r * 0.45f, s * 0.42f, lv_color_mix(lv_color_white(), c, 110));
    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

lv_obj_t *ui_icon_snow(lv_obj_t *parent, int s, lv_color_t c)
{
    lv_layer_t layer;
    lv_obj_t *cv = canvas_new(parent, s, &layer);
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = c;
    ld.width = s / 11 > 1 ? s / 11 : 1;
    ld.round_start = ld.round_end = 1;
    const float cx = s / 2.0f, cy = s / 2.0f, R = s * 0.44f;
    for (int i = 0; i < 6; i++) {
        float a = (float)M_PI / 3.0f * i;
        float ex = cx + R * cosf(a), ey = cy + R * sinf(a);
        ld.p1.x = cx; ld.p1.y = cy; ld.p2.x = ex; ld.p2.y = ey;
        lv_draw_line(&layer, &ld);
        /* Zweige */
        float bx = cx + R * 0.6f * cosf(a), by = cy + R * 0.6f * sinf(a);
        for (int k = -1; k <= 1; k += 2) {
            float b = a + k * (float)M_PI / 4.0f;
            ld.p1.x = bx; ld.p1.y = by;
            ld.p2.x = bx + R * 0.32f * cosf(b); ld.p2.y = by + R * 0.32f * sinf(b);
            lv_draw_line(&layer, &ld);
        }
    }
    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

static void line(lv_layer_t *layer, lv_draw_line_dsc_t *ld, float x1, float y1, float x2, float y2)
{
    ld->p1.x = x1; ld->p1.y = y1; ld->p2.x = x2; ld->p2.y = y2;
    lv_draw_line(layer, ld);
}

static void line_init(lv_draw_line_dsc_t *ld, int s, lv_color_t c, int div)
{
    lv_draw_line_dsc_init(ld);
    ld->color = c;
    ld->width = s / div > 1 ? s / div : 1;
    ld->round_start = ld->round_end = 1;
}

/* Sonne (Photovoltaik) */
lv_obj_t *ui_icon_sun(lv_obj_t *parent, int s, lv_color_t c)
{
    lv_layer_t layer;
    lv_obj_t *cv = canvas_new(parent, s, &layer);
    const float cx = s / 2.0f, cy = s / 2.0f;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = c;
    rd.radius = LV_RADIUS_CIRCLE;
    float r = s * 0.22f;
    lv_area_t a = { (int32_t)(cx - r), (int32_t)(cy - r), (int32_t)(cx + r), (int32_t)(cy + r) };
    lv_draw_rect(&layer, &rd, &a);
    lv_draw_line_dsc_t ld;
    line_init(&ld, s, c, 12);
    for (int i = 0; i < 8; i++) {
        float an = (float)M_PI / 4.0f * i;
        line(&layer, &ld, cx + s * 0.32f * cosf(an), cy + s * 0.32f * sinf(an),
             cx + s * 0.45f * cosf(an), cy + s * 0.45f * sinf(an));
    }
    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

/* Strommast (Netz) */
lv_obj_t *ui_icon_pylon(lv_obj_t *parent, int s, lv_color_t c)
{
    lv_layer_t layer;
    lv_obj_t *cv = canvas_new(parent, s, &layer);
    lv_draw_line_dsc_t ld;
    line_init(&ld, s, c, 14);
    const float cx = s / 2.0f;
    float top = s * 0.06f, bot = s * 0.94f;
    /* Beine */
    line(&layer, &ld, cx, top, cx - s * 0.30f, bot);
    line(&layer, &ld, cx, top, cx + s * 0.30f, bot);
    /* Traversen */
    line(&layer, &ld, cx - s * 0.40f, s * 0.30f, cx + s * 0.40f, s * 0.30f);
    line(&layer, &ld, cx - s * 0.30f, s * 0.52f, cx + s * 0.30f, s * 0.52f);
    /* Streben */
    line(&layer, &ld, cx - s * 0.11f, s * 0.30f, cx + s * 0.19f, s * 0.52f);
    line(&layer, &ld, cx + s * 0.11f, s * 0.30f, cx - s * 0.19f, s * 0.52f);
    line(&layer, &ld, cx - s * 0.19f, s * 0.52f, cx + s * 0.26f, s * 0.82f);
    line(&layer, &ld, cx + s * 0.19f, s * 0.52f, cx - s * 0.26f, s * 0.82f);
    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

/* Auto (Wallbox) */
lv_obj_t *ui_icon_car(lv_obj_t *parent, int s, lv_color_t c)
{
    lv_layer_t layer;
    lv_obj_t *cv = canvas_new(parent, s, &layer);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = c;
    rd.radius = s / 10;
    lv_area_t body = { (int32_t)(s * 0.06f), (int32_t)(s * 0.45f), (int32_t)(s * 0.94f), (int32_t)(s * 0.72f) };
    lv_draw_rect(&layer, &rd, &body);
    rd.radius = s / 6;
    lv_area_t cab = { (int32_t)(s * 0.24f), (int32_t)(s * 0.24f), (int32_t)(s * 0.76f), (int32_t)(s * 0.55f) };
    lv_draw_rect(&layer, &rd, &cab);
    /* Räder */
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_color = lv_color_black();
    rd.bg_opa = LV_OPA_COVER;
    float r = s * 0.12f;
    for (int i = 0; i < 2; i++) {
        float wx = s * (i ? 0.72f : 0.28f), wy = s * 0.72f;
        lv_area_t w = { (int32_t)(wx - r), (int32_t)(wy - r), (int32_t)(wx + r), (int32_t)(wy + r) };
        rd.bg_color = c;
        lv_draw_rect(&layer, &rd, &w);
        lv_area_t h = { (int32_t)(wx - r * 0.45f), (int32_t)(wy - r * 0.45f), (int32_t)(wx + r * 0.45f),
                        (int32_t)(wy + r * 0.45f) };
        rd.bg_color = lv_color_hex(0x1e2126);
        lv_draw_rect(&layer, &rd, &h);
    }
    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

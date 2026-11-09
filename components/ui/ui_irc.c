/*
 * Intelligente Raumregelung (IRoomControllerV2).
 *
 * Status laut Loxone "Structure File" (2025-06-03):
 *   tempActual, tempTarget, comfortTemperature, comfortTemperatureCool, comfortTemperatureOffset,
 *   absentMinOffset, absentMaxOffset, frostProtectTemperature, heatProtectTemperature,
 *   activeMode (0 Eco, 1 Komfort, 2 Gebäudeschutz, 3 Manuell, 4 Aus),
 *   operatingMode (0 Auto H+K, 1 Auto H, 2 Auto K, 3 Manuell H+K, 4 Manuell H, 5 Manuell K, -1 Aus),
 *   overrideEntries (Text), humidityActual, co2.
 * Befehle: setComfortModeTemp/{t} (vorübergehend, ±0,5), setOperatingMode/{m},
 *   setComfortTemperature/{t}, setComfortTemperatureCool/{t},
 *   setAbsentMinTemperature/{offset}, setAbsentMaxTemperature/{offset}.
 * Zeitplan: Unterbaustein IRCV2Daytimer, Status entriesAndDefaultValue + mode (heutige Betriebsart).
 *   Eintragswert = Temperatur-ID (0 Eco, 1 Komfort, 2 Gebäudeschutz) oder, wenn Bit 4 gesetzt,
 *   eigene Temperatur (Bits 8..23 = Temperatur * 10).
 */

#include "ui_irc.h"
#include "ui_priv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lox_client.h"
#include "lox_struct.h"
#include "net_time.h"

static const char *TAG = "ui_irc";

typedef struct {
    lox_uuid_t u;
    bool ok;
} st_t;

#define MAX_LINKS 4
#define MAX_DT    32

struct ui_irc {
    bool detail;
    char uuid[48];
    char name[48];
    char fmt[16];
    st_t act, target, comfort, comfort_cool, offset, abs_min, abs_max, frost, heat;
    st_t amode, opmode, overrides, hum, co2, dt_entries, dt_mode;
    /* verlinkte Bausteine (Anzeige-Werte) */
    int nlinks;
    char link_name[MAX_LINKS][40];
    char link_fmt[MAX_LINKS][16];
    st_t link_val[MAX_LINKS];
    lv_obj_t *link_lbl[MAX_LINKS];

    /* Kachel */
    lv_obj_t *t_int, *t_dec, *mode_box, *mode_lbl;
    int icon_kind;                      // 0 keins, 1 Flamme, 2 Schneeflocke
    /* Detail */
    lv_obj_t *main, *settings;
    lv_obj_t *op_lbl, *op_row, *sched_lbl, *target_lbl, *target_kind;
    lv_obj_t *bar, *bar_now, *bar_ticks, *now_lbl, *seg_info;
    lv_obj_t *hum_val, *co2_val, *cond_box;
    uint32_t dt_hash;
    /* Einstellungen */
    lv_obj_t *ladder, *sel_name, *sel_val;
    int sel;                            // 0 Komfort H, 1 Eco H, 2 Komfort K, 3 Eco K
    lv_obj_t *mark[6];
};

/* ---------------- Hilfen ---------------- */

static void st(const cJSON *ctrl, const char *name, st_t *out)
{
    const cJSON *s = cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "states"), name);
    out->ok = cJSON_IsString(s) && lox_uuid_parse(s->valuestring, &out->u);
}

static bool val(const st_t *s, double *v)
{
    return s->ok && lox_value_get(&s->u, v);
}

static void send(ui_irc_t *r, const char *cmd)
{
    ESP_LOGI(TAG, "%s: %s", r->name, cmd);
    lox_send_command(r->uuid, cmd);
}

static const char *amode_text(int m)
{
    switch (m) {
    case 0: return "Eco";
    case 1: return "Komfort";
    case 2: return "Gebäudeschutz";
    case 3: return "Manuell";
    case 4: return "Aus";
    default: return "";
    }
}

static const char *opmode_text(int m)
{
    switch (m) {
    case 0: case 1: case 2: return "AUTO";
    case 3: case 4: case 5: return "MANUELL";
    case -1: return "AUS";
    default: return "-";
    }
}

/* Heizen (1), Kühlen (2) oder nichts (0) aus Betriebsart und Temperaturen */
static int climate_kind(ui_irc_t *r)
{
    double op = 0, act = 0, tgt = 0;
    val(&r->opmode, &op);
    if ((int)op == 1 || (int)op == 4) return 1;
    if ((int)op == 2 || (int)op == 5) return 2;
    if ((int)op == -1) return 0;
    if (val(&r->act, &act) && val(&r->target, &tgt)) return act > tgt + 0.3 ? 2 : 1;
    return 1;
}

static uint32_t kind_color(int k)
{
    return k == 1 ? UI_COL_HEAT : k == 2 ? UI_COL_COOL : UI_COL_TEXT;
}

static void load(ui_irc_t *r, const cJSON *ctrl)
{
    const cJSON *u = cJSON_GetObjectItem(ctrl, "uuidAction");
    const cJSON *n = cJSON_GetObjectItem(ctrl, "name");
    strlcpy(r->uuid, cJSON_IsString(u) ? u->valuestring : "", sizeof(r->uuid));
    strlcpy(r->name, cJSON_IsString(n) ? n->valuestring : "?", sizeof(r->name));
    const cJSON *f = cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "details"), "format");
    strlcpy(r->fmt, cJSON_IsString(f) ? f->valuestring : "%.1f°", sizeof(r->fmt));

    st(ctrl, "tempActual", &r->act);
    st(ctrl, "tempTarget", &r->target);
    st(ctrl, "comfortTemperature", &r->comfort);
    st(ctrl, "comfortTemperatureCool", &r->comfort_cool);
    st(ctrl, "comfortTemperatureOffset", &r->offset);
    st(ctrl, "absentMinOffset", &r->abs_min);
    st(ctrl, "absentMaxOffset", &r->abs_max);
    st(ctrl, "frostProtectTemperature", &r->frost);
    st(ctrl, "heatProtectTemperature", &r->heat);
    st(ctrl, "activeMode", &r->amode);
    st(ctrl, "operatingMode", &r->opmode);
    st(ctrl, "overrideEntries", &r->overrides);
    st(ctrl, "humidityActual", &r->hum);
    st(ctrl, "co2", &r->co2);

    /* Zeitplan-Unterbaustein */
    const cJSON *sc;
    cJSON_ArrayForEach(sc, cJSON_GetObjectItem(ctrl, "subControls")) {
        const cJSON *t = cJSON_GetObjectItem(sc, "type");
        if (cJSON_IsString(t) && strstr(t->valuestring, "Daytimer")) {
            st(sc, "entriesAndDefaultValue", &r->dt_entries);
            st(sc, "mode", &r->dt_mode);
        }
    }

    /* Verlinkte Bausteine (z. B. Luftfeuchte-Sensor) */
    const cJSON *l;
    cJSON_ArrayForEach(l, cJSON_GetObjectItem(ctrl, "links")) {
        if (!cJSON_IsString(l) || r->nlinks >= MAX_LINKS) continue;
        const cJSON *lc = lox_struct_find_control(l->valuestring, NULL);
        if (lc == NULL) continue;
        const cJSON *ln = cJSON_GetObjectItem(lc, "name");
        const cJSON *lf = cJSON_GetObjectItem(cJSON_GetObjectItem(lc, "details"), "format");
        int i = r->nlinks++;
        strlcpy(r->link_name[i], cJSON_IsString(ln) ? ln->valuestring : "?", sizeof(r->link_name[i]));
        strlcpy(r->link_fmt[i], cJSON_IsString(lf) ? lf->valuestring : "%.1f", sizeof(r->link_fmt[i]));
        st(lc, "value", &r->link_val[i]);
        if (!r->link_val[i].ok) st(lc, "active", &r->link_val[i]);
    }
}

/* Format "%.1f°" sicher anwenden */
static void fmtv(const char *fmt, double v, char *out, size_t size)
{
    const char *p = strchr(fmt, '%');
    if (p && !strchr(p + 1, '%') && strpbrk(p, "fg")) snprintf(out, size, fmt, v);
    else if (p && p[1] == '.' && strstr(p, "%%")) snprintf(out, size, fmt, v);
    else snprintf(out, size, "%.1f", v);
}

/* "20.9" -> "20" und ".9" */
static void split_temp(double v, char *ip, size_t is, char *dp, size_t ds)
{
    double a = fabs(v);
    int whole = (int)floor(a + 0.05);
    int dec = (int)lround((a - whole) * 10);
    if (dec >= 10) { whole++; dec = 0; }
    snprintf(ip, is, "%s%d", v < 0 ? "-" : "", whole);
    snprintf(dp, ds, ".%d", dec < 0 ? 0 : dec);
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static lv_obj_t *label(lv_obj_t *parent, ui_font_id_t f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, ui_font(f), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *flat_btn(lv_obj_t *parent, const char *txt, ui_font_id_t f)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x161a1f), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, ui_px(12), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, ui_font(f), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

/* Symbol in einem kleinen Container austauschen */
static void set_icon(lv_obj_t *holder, int *cur, int kind, int size)
{
    if (*cur == kind) return;
    *cur = kind;
    lv_obj_clean(holder);
    if (kind == 1) ui_icon_flame(holder, size, lv_color_hex(UI_COL_HEAT));
    else if (kind == 2) ui_icon_snow(holder, size, lv_color_hex(UI_COL_COOL));
}

static void step_cb(lv_event_t *e)
{
    ui_irc_t *r = lv_event_get_user_data(e);
    int dir = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    double comfort = 0, offset = 0;
    if (!val(&r->comfort, &comfort)) return;
    val(&r->offset, &offset);
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "setComfortModeTemp/%.1f", comfort + offset + dir * 0.5);
    send(r, cmd);
}

static lv_obj_t *step_btn(lv_obj_t *parent, ui_irc_t *r, int dir, int w, int h)
{
    lv_obj_t *b = flat_btn(parent, dir < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS, UI_F_M);
    lv_obj_set_size(b, w, h);
    lv_obj_set_user_data(b, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(b, step_cb, LV_EVENT_CLICKED, r);
    return b;
}

/* ---------------- Kachel ---------------- */

ui_irc_t *ui_irc_tile_create(lv_obj_t *tile, const cJSON *ctrl, const char *title, int w, int h)
{
    ui_irc_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM);
    if (r == NULL) return NULL;
    load(r, ctrl);
    const bool big = h >= 2;

    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tile, ui_px(12), 0);
    lv_obj_set_style_pad_row(tile, ui_px(4), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1e2126), 0);

    /* oben: Temperatur links, Name rechts */
    lv_obj_t *top = box(tile);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(top, ui_px(6), 0);
    lv_obj_t *temp = box(top);
    lv_obj_set_size(temp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(temp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    r->t_int = label(temp, big ? UI_F_XXL : UI_F_XL, UI_COL_TEXT, "-");
    r->t_dec = label(temp, big ? UI_F_XL : UI_F_L, UI_COL_DIM, "");
    lv_obj_set_style_pad_top(r->t_dec, ui_px(big ? 14 : 6), 0);
    lv_obj_t *nm = label(top, UI_F_S, UI_COL_DIM, title && title[0] ? title : r->name);
    lv_obj_set_flex_grow(nm, 1);
    lv_obj_set_height(nm, ui_px(20));                   // eine Zeile, sonst "..."
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_RIGHT, 0);

    /* Modus mit Symbol */
    lv_obj_t *mrow = box(tile);
    lv_obj_set_size(mrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mrow, ui_px(6), 0);
    r->mode_box = box(mrow);
    lv_obj_set_size(r->mode_box, ui_px(26), ui_px(26));
    r->mode_lbl = label(mrow, UI_F_M, UI_COL_TEXT, "");

    lv_obj_t *sp = box(tile);
    lv_obj_set_flex_grow(sp, 1);

    /* unten: − und + über die ganze Breite */
    lv_obj_t *btns = box(tile);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, ui_px(8), 0);
    int bh = ui_px(big ? 64 : 46);
    lv_obj_t *m = step_btn(btns, r, -1, 10, bh);
    lv_obj_t *p = step_btn(btns, r, +1, 10, bh);
    lv_obj_set_flex_grow(m, 1);
    lv_obj_set_flex_grow(p, 1);
    (void)w;

    ui_irc_update(r);
    return r;
}

static void tile_update(ui_irc_t *r)
{
    double v, mode = -1;
    if (val(&r->act, &v)) {
        char ip[12], dp[6];
        split_temp(v, ip, sizeof(ip), dp, sizeof(dp));
        lv_label_set_text_fmt(r->t_int, "%s°", ip);
        lv_label_set_text(r->t_dec, dp);
    }
    int k = climate_kind(r);
    lv_obj_set_style_text_color(r->t_int, lv_color_hex(kind_color(k)), 0);
    lv_obj_set_style_text_color(r->t_dec, lv_color_mix(lv_color_hex(kind_color(k)), lv_color_black(), 150), 0);
    val(&r->amode, &mode);
    lv_label_set_text(r->mode_lbl, amode_text((int)mode));
    set_icon(r->mode_box, &r->icon_kind, k, ui_px(26));
}

/* ---------------- Detail ---------------- */

static void open_settings(lv_event_t *e);
static void close_settings(lv_event_t *e);

static void op_chip_cb(lv_event_t *e)
{
    ui_irc_t *r = lv_event_get_user_data(e);
    if (lv_obj_has_flag(r->op_row, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(r->op_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(r->op_row, LV_OBJ_FLAG_HIDDEN);
}

static void op_select_cb(lv_event_t *e)
{
    ui_irc_t *r = lv_event_get_user_data(e);
    int m = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "setOperatingMode/%d", m);
    send(r, cmd);
    lv_obj_add_flag(r->op_row, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *chip(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2b2f35), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, ui_px(10), 0);
    lv_obj_set_style_pad_hor(b, ui_px(14), 0);
    lv_obj_set_height(b, ui_px(48));
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, ui_px(8), 0);
    (void)txt;
    return b;
}

static void build_main(ui_irc_t *r, lv_obj_t *parent, const char *room)
{
    r->main = box(parent);
    lv_obj_set_size(r->main, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(r->main, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r->main, ui_px(20), 0);

    /* linke Spalte */
    lv_obj_t *left = box(r->main);
    lv_obj_set_size(left, LV_PCT(58), LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, ui_px(10), 0);

    label(left, UI_F_S, UI_COL_DIM, room && room[0] ? room : r->name);
    lv_obj_t *temp = box(left);
    lv_obj_set_size(temp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(temp, LV_FLEX_FLOW_ROW);
    r->t_int = label(temp, UI_F_XXL, UI_COL_TEXT, "-");
    r->t_dec = label(temp, UI_F_XL, UI_COL_DIM, "");
    lv_obj_set_style_pad_top(r->t_dec, ui_px(18), 0);

    /* Chips: Betriebsart, Zeitplan */
    lv_obj_t *chips = box(left);
    lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, ui_px(10), 0);
    lv_obj_t *c1 = chip(chips, "");
    r->mode_box = box(c1);
    lv_obj_set_size(r->mode_box, ui_px(22), ui_px(22));
    r->op_lbl = label(c1, UI_F_S, UI_COL_HEAT, "AUTO");
    label(c1, UI_F_S, UI_COL_DIM, LV_SYMBOL_RIGHT);
    lv_obj_add_event_cb(c1, op_chip_cb, LV_EVENT_CLICKED, r);
    lv_obj_t *c2 = chip(chips, "");
    lv_obj_remove_flag(c2, LV_OBJ_FLAG_CLICKABLE);
    r->sched_lbl = label(c2, UI_F_S, UI_COL_TEXT, "Nach Zeitplan");

    r->op_row = box(left);
    lv_obj_set_size(r->op_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->op_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r->op_row, ui_px(8), 0);
    lv_obj_set_style_pad_row(r->op_row, ui_px(8), 0);
    static const int ops[] = { 0, 1, 2, 3 };
    static const char *opn[] = { "Automatik", "Nur heizen", "Nur kühlen", "Manuell" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = flat_btn(r->op_row, opn[i], UI_F_S);
        lv_obj_set_height(b, ui_px(46));
        lv_obj_set_style_pad_hor(b, ui_px(14), 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)ops[i]);
        lv_obj_add_event_cb(b, op_select_cb, LV_EVENT_CLICKED, r);
    }
    lv_obj_add_flag(r->op_row, LV_OBJ_FLAG_HIDDEN);

    /* Soll-Temperatur mit − / + und Zeitplan */
    lv_obj_t *card = box(left);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x262a30), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, ui_px(14), 0);
    lv_obj_set_style_pad_all(card, ui_px(12), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, ui_px(10), 0);

    lv_obj_t *tr = box(card);
    lv_obj_set_size(tr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    step_btn(tr, r, -1, ui_px(150), ui_px(62));
    lv_obj_t *tc = box(tr);
    lv_obj_set_size(tc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tc, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    r->target_lbl = label(tc, UI_F_L, UI_COL_TEXT, "-");
    r->target_kind = label(tc, UI_F_S, UI_COL_DIM, "");
    step_btn(tr, r, +1, ui_px(150), ui_px(62));

    /* Zeitplan-Balken 00..24 */
    r->now_lbl = label(card, UI_F_S, UI_COL_TEXT, "");
    lv_obj_set_width(r->now_lbl, LV_PCT(100));
    r->bar = box(card);
    lv_obj_set_size(r->bar, LV_PCT(100), ui_px(10));
    lv_obj_set_style_bg_color(r->bar, lv_color_hex(0x40454c), 0);
    lv_obj_set_style_bg_opa(r->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r->bar, ui_px(5), 0);
    r->bar_ticks = box(card);
    lv_obj_set_size(r->bar_ticks, LV_PCT(100), ui_px(22));
    r->seg_info = label(card, UI_F_S, UI_COL_TEXT, "");

    /* rechte Spalte: Raumbedingungen, verlinkte Objekte, Einstellungen */
    lv_obj_t *right = box(r->main);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, ui_px(10), 0);
    lv_obj_set_style_pad_top(right, ui_px(28), 0);

    label(right, UI_F_S, UI_COL_DIM, "Raumbedingungen");
    r->cond_box = box(right);
    lv_obj_set_size(r->cond_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->cond_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r->cond_box, ui_px(10), 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = box(r->cond_box);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x262a30), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, ui_px(12), 0);
        lv_obj_set_style_pad_all(c, ui_px(12), 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        label(c, UI_F_S, UI_COL_DIM, i == 0 ? "Feuchte" : "CO2");
        lv_obj_t *v = label(c, UI_F_L, UI_COL_TEXT, "-");
        if (i == 0) r->hum_val = v; else r->co2_val = v;
    }

    if (r->nlinks) {
        label(right, UI_F_S, UI_COL_DIM, "Verlinkte Objekte");
        for (int i = 0; i < r->nlinks; i++) {
            lv_obj_t *c = box(right);
            lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(c, lv_color_hex(0x262a30), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(c, ui_px(12), 0);
            lv_obj_set_style_pad_all(c, ui_px(12), 0);
            lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
            r->link_lbl[i] = label(c, UI_F_M, UI_COL_TEXT, "-");
            label(c, UI_F_S, UI_COL_DIM, r->link_name[i]);
        }
    }

    lv_obj_t *sp = box(right);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_t *sb = flat_btn(right, LV_SYMBOL_SETTINGS "  Einstellungen", UI_F_S);
    lv_obj_set_size(sb, LV_PCT(100), ui_px(52));
    lv_obj_add_event_cb(sb, open_settings, LV_EVENT_CLICKED, r);
}

/* Zeitplan: heutige Einträge der aktuellen Betriebsart */
static bool is_comfort(double v)
{
    uint32_t n = (uint32_t)v;
    if (n & 0x10) return true;                  // eigene Temperatur (Komfort-ähnlich)
    return n == 1;
}

static void hm(int min, char *out, size_t size)
{
    snprintf(out, size, "%02d:%02d", (min / 60) % 24, min % 60);
}

static void update_schedule(ui_irc_t *r)
{
    if (!r->dt_entries.ok) return;
    lox_dt_entry_t e[MAX_DT];
    double def = 0, mode = -1;
    int n = lox_daytimer_get(&r->dt_entries.u, &def, e, MAX_DT);
    if (n < 0) return;
    val(&r->dt_mode, &mode);

    /* heutige Einträge: Betriebsart = Status "mode"; wenn keiner passt, alle mit kleinster Betriebsart */
    lox_dt_entry_t today[MAX_DT];
    int t = 0;
    for (int i = 0; i < n; i++) if (e[i].mode == (int)mode) today[t++] = e[i];
    uint32_t h = (uint32_t)t * 2654435761u;
    for (int i = 0; i < t; i++) h ^= (uint32_t)(today[i].from_min * 31 + today[i].to_min) * 2246822519u + (uint32_t)today[i].value;

    if (h != r->dt_hash) {
        r->dt_hash = h;
        lv_obj_clean(r->bar);
        lv_obj_clean(r->bar_ticks);
        for (int i = 0; i < t; i++) {
            if (!is_comfort(today[i].value)) continue;
            lv_obj_t *s = box(r->bar);
            lv_obj_set_style_bg_color(s, lv_color_hex(0x8be04e), 0);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s, ui_px(5), 0);
            lv_obj_set_height(s, LV_PCT(100));
            lv_obj_set_x(s, lv_pct(today[i].from_min * 100 / 1440));
            lv_obj_set_width(s, lv_pct((today[i].to_min - today[i].from_min) * 100 / 1440));
        }
        /* Beschriftung: 00, Grenzen der Einträge, 24 */
        int marks[2 * MAX_DT + 2], nm = 0;
        marks[nm++] = 0;
        for (int i = 0; i < t; i++) {
            if (!is_comfort(today[i].value)) continue;
            marks[nm++] = today[i].from_min;
            marks[nm++] = today[i].to_min;
        }
        marks[nm++] = 1440;
        for (int i = 0; i < nm; i++) {
            lv_obj_t *l = label(r->bar_ticks, UI_F_S, UI_COL_DIM, "");
            lv_label_set_text_fmt(l, "%02d", marks[i] / 60);
            lv_obj_set_x(l, lv_pct(marks[i] * 100 / 1440));
            if (marks[i] >= 1440) lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, 0);
        }
        r->bar_now = box(r->bar);
        lv_obj_set_size(r->bar_now, ui_px(3), LV_PCT(100));
        lv_obj_set_style_bg_color(r->bar_now, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(r->bar_now, LV_OPA_COVER, 0);
    }

    if (!net_time_valid()) {
        lv_label_set_text(r->now_lbl, "");
        lv_label_set_text(r->seg_info, "");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int cur = tm.tm_hour * 60 + tm.tm_min;
    static const char *wd[] = { "So.", "Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa." };
    lv_label_set_text_fmt(r->now_lbl, "%s %02d:%02d", wd[tm.tm_wday], tm.tm_hour, tm.tm_min);
    lv_obj_set_style_pad_left(r->now_lbl, lv_obj_get_width(r->bar) * cur / 1440 > ui_px(60)
                              ? lv_obj_get_width(r->bar) * cur / 1440 - ui_px(40) : 0, 0);
    if (r->bar_now) lv_obj_set_x(r->bar_now, lv_pct(cur * 100 / 1440));

    char buf[8];
    int next_start = -1;
    for (int i = 0; i < t; i++) {
        if (!is_comfort(today[i].value)) continue;
        if (cur >= today[i].from_min && cur < today[i].to_min) {
            hm(today[i].to_min, buf, sizeof(buf));
            lv_label_set_text_fmt(r->seg_info, LV_SYMBOL_OK "  Komfort bis %s", buf);
            lv_obj_set_style_text_color(r->seg_info, lv_color_hex(0x8be04e), 0);
            return;
        }
        if (today[i].from_min > cur && (next_start < 0 || today[i].from_min < next_start)) next_start = today[i].from_min;
    }
    if (next_start >= 0) {
        hm(next_start, buf, sizeof(buf));
        lv_label_set_text_fmt(r->seg_info, "Eco bis %s", buf);
    } else {
        lv_label_set_text(r->seg_info, "Eco bis morgen");
    }
    lv_obj_set_style_text_color(r->seg_info, lv_color_hex(UI_COL_DIM), 0);
}

static void detail_update(ui_irc_t *r)
{
    double v, op = 0, tgt;
    char ip[12], dp[6], buf[24];
    int k = climate_kind(r);
    if (val(&r->act, &v)) {
        split_temp(v, ip, sizeof(ip), dp, sizeof(dp));
        lv_label_set_text_fmt(r->t_int, "%s°", ip);
        lv_label_set_text(r->t_dec, dp);
    }
    lv_obj_set_style_text_color(r->t_int, lv_color_hex(kind_color(k)), 0);
    lv_obj_set_style_text_color(r->t_dec, lv_color_mix(lv_color_hex(kind_color(k)), lv_color_black(), 150), 0);
    val(&r->opmode, &op);
    lv_label_set_text(r->op_lbl, opmode_text((int)op));
    lv_obj_set_style_text_color(r->op_lbl, lv_color_hex(kind_color(k)), 0);
    set_icon(r->mode_box, &r->icon_kind, k, ui_px(22));

    if (val(&r->target, &tgt)) {
        fmtv(r->fmt, tgt, buf, sizeof(buf));
        lv_label_set_text(r->target_lbl, buf);
        lv_label_set_text(r->target_kind, k == 2 ? "MAX" : "MIN");
    }
    char ov[64] = "";
    bool override = r->overrides.ok && lox_text_get(&r->overrides.u, ov, sizeof(ov)) && strlen(ov) > 4;
    lv_label_set_text(r->sched_lbl, override ? "Timer aktiv" : "Nach Zeitplan");

    double hum = 0, co2 = 0;
    bool hh = val(&r->hum, &hum) && hum > 0, hc = val(&r->co2, &co2) && co2 > 0;
    lv_label_set_text_fmt(r->hum_val, hh ? "%.0f%%" : "-", hum);
    lv_label_set_text_fmt(r->co2_val, hc ? "%.0f ppm" : "-", co2);
    lv_obj_t *c2 = lv_obj_get_child(r->cond_box, 1);
    if (hc) lv_obj_remove_flag(c2, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(c2, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < r->nlinks; i++) {
        if (val(&r->link_val[i], &v)) {
            fmtv(r->link_fmt[i], v, buf, sizeof(buf));
            lv_label_set_text(r->link_lbl[i], buf);
        }
    }
    update_schedule(r);
}

/* ---------------- Einstellungen (Temperatur-Leiter) ---------------- */

typedef struct {
    const char *name;
    double v;
    bool cool;
    bool editable;
    bool left;          // links von der Leiter (Komfort/Eco), rechts: Schutz
} ladder_item_t;

static void ladder_values(ui_irc_t *r, ladder_item_t it[6])
{
    double c = 21, cc = 25, mn = 3, mx = 3, fr = 5, he = 28;
    val(&r->comfort, &c);
    val(&r->comfort_cool, &cc);
    val(&r->abs_min, &mn);
    val(&r->abs_max, &mx);
    val(&r->frost, &fr);
    val(&r->heat, &he);
    it[0] = (ladder_item_t){ "Komfort", c, false, true, true };
    it[1] = (ladder_item_t){ "Eco", c - mn, false, true, true };
    it[2] = (ladder_item_t){ "Komfort", cc, true, true, true };
    it[3] = (ladder_item_t){ "Eco", cc + mx, true, true, true };
    it[4] = (ladder_item_t){ "Frostschutz", fr, false, false, false };
    it[5] = (ladder_item_t){ "Hitzeschutz", he, true, false, false };
}

static void sel_cb(lv_event_t *e)
{
    ui_irc_t *r = lv_event_get_user_data(e);
    r->sel = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    ui_irc_update(r);
}

static void adjust_cb(lv_event_t *e)
{
    ui_irc_t *r = lv_event_get_user_data(e);
    int dir = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    ladder_item_t it[6];
    ladder_values(r, it);
    double nv = it[r->sel].v + dir * 0.5;
    char cmd[48];
    switch (r->sel) {
    case 0: snprintf(cmd, sizeof(cmd), "setComfortTemperature/%.1f", nv); break;
    case 1: snprintf(cmd, sizeof(cmd), "setAbsentMinTemperature/%.1f", it[0].v - nv); break;
    case 2: snprintf(cmd, sizeof(cmd), "setComfortTemperatureCool/%.1f", nv); break;
    case 3: snprintf(cmd, sizeof(cmd), "setAbsentMaxTemperature/%.1f", nv - it[2].v); break;
    default: return;
    }
    send(r, cmd);
}

static void build_settings(ui_irc_t *r, lv_obj_t *parent)
{
    r->settings = box(parent);
    lv_obj_set_size(r->settings, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(r->settings, LV_OBJ_FLAG_HIDDEN);

    /* Leiter links */
    r->ladder = box(r->settings);
    lv_obj_set_size(r->ladder, LV_PCT(60), LV_PCT(100));

    /* Bearbeiten rechts */
    lv_obj_t *ed = box(r->settings);
    lv_obj_set_size(ed, LV_PCT(36), LV_PCT(100));
    lv_obj_align(ed, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(ed, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ed, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ed, ui_px(14), 0);
    r->sel_name = label(ed, UI_F_M, UI_COL_DIM, "");
    r->sel_val = label(ed, UI_F_XL, UI_COL_TEXT, "");
    lv_obj_t *row = box(ed);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, ui_px(10), 0);
    for (int d = -1; d <= 1; d += 2) {
        lv_obj_t *b = flat_btn(row, d < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS, UI_F_M);
        lv_obj_set_height(b, ui_px(60));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_user_data(b, (void *)(intptr_t)d);
        lv_obj_add_event_cb(b, adjust_cb, LV_EVENT_CLICKED, r);
    }
    label(ed, UI_F_S, UI_COL_DIM, "Wert links antippen zum Ändern.\nFrost-/Hitzeschutz nur in Loxone Config.");
    lv_obj_t *back = flat_btn(ed, LV_SYMBOL_LEFT "  Zurück", UI_F_S);
    lv_obj_set_size(back, LV_PCT(100), ui_px(52));
    lv_obj_add_event_cb(back, close_settings, LV_EVENT_CLICKED, r);
}

static void ladder_update(ui_irc_t *r)
{
    ladder_item_t it[6];
    ladder_values(r, it);
    uint32_t h = 2166136261u ^ (uint32_t)r->sel;
    for (int i = 0; i < 6; i++) h = (h ^ (uint32_t)lround(it[i].v * 10)) * 16777619u;
    if (h == r->dt_hash && lv_obj_get_child_count(r->ladder)) return;
    r->dt_hash = h;
    lv_obj_clean(r->ladder);
    lv_obj_update_layout(r->ladder);
    const int H = lv_obj_get_height(r->ladder), W = lv_obj_get_width(r->ladder);
    const double lo = it[4].v - 1, hi = it[5].v + 1;
    const int pad = ui_px(30), barx = W / 2, barw = ui_px(14);
    #define YPOS(t) (pad + (int)((hi - (t)) / (hi - lo) * (H - 2 * pad)))

    lv_obj_t *bg = box(r->ladder);
    lv_obj_set_pos(bg, barx - barw / 2, pad);
    lv_obj_set_size(bg, barw, H - 2 * pad);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x262a30), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bg, barw / 2, 0);
    /* Heizbereich: Frostschutz bis Komfort Heizen; Kühlbereich: Komfort Kühlen bis Hitzeschutz */
    struct { double a, b; uint32_t c1, c2; } seg[2] = {
        { it[0].v, it[4].v, 0x8a4b16, 0x3a2410 },
        { it[5].v, it[2].v, 0x1d2a44, 0x3a9ad0 },
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *s = box(r->ladder);
        int y1 = YPOS(seg[i].a), y2 = YPOS(seg[i].b);
        lv_obj_set_pos(s, barx - barw / 2, y1);
        lv_obj_set_size(s, barw, y2 - y1 > 2 ? y2 - y1 : 2);
        lv_obj_set_style_bg_color(s, lv_color_hex(seg[i].c1), 0);
        lv_obj_set_style_bg_grad_color(s, lv_color_hex(seg[i].c2), 0);
        lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s, barw / 2, 0);
    }

    /* Markierungen; links überlappungsfrei verteilen */
    int last_y = -1000;
    int order[4] = { 3, 2, 0, 1 };          // von oben nach unten: Eco K, Komfort K, Komfort H, Eco H
    for (int oi = 0; oi < 6; oi++) {
        int i = oi < 4 ? order[oi] : oi;
        int y = YPOS(it[i].v);
        int bw = ui_px(200), bh = ui_px(60);
        lv_obj_t *b = lv_button_create(r->ladder);
        lv_obj_set_size(b, it[i].left ? bw : ui_px(170), bh);
        int by = y - bh / 2;
        if (it[i].left) {
            if (by < last_y + bh + ui_px(6)) by = last_y + bh + ui_px(6);
            last_y = by;
        }
        lv_obj_set_pos(b, it[i].left ? barx - barw - bw - ui_px(10) : barx + barw + ui_px(14), by);
        bool selected = it[i].editable && r->sel == i;
        lv_obj_set_style_bg_color(b, lv_color_hex(selected ? 0x3a4250 : 0x262a30), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, ui_px(10), 0);
        lv_obj_set_style_border_width(b, selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_COL_ON), 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, ui_px(8), 0);
        if (it[i].left) {
            lv_obj_t *ib = box(b);
            lv_obj_set_size(ib, ui_px(22), ui_px(22));
            if (it[i].cool) ui_icon_snow(ib, ui_px(22), lv_color_hex(UI_COL_COOL));
            else ui_icon_flame(ib, ui_px(22), lv_color_hex(UI_COL_HEAT));
        }
        char buf[24];
        lv_obj_t *col = box(b);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        fmtv(r->fmt, it[i].v, buf, sizeof(buf));
        label(col, UI_F_M, UI_COL_TEXT, buf);
        label(col, UI_F_S, it[i].name[0] == 'K' ? 0x8be04e : UI_COL_DIM, it[i].name);
        if (it[i].editable) {
            lv_obj_set_user_data(b, (void *)(intptr_t)i);
            lv_obj_add_event_cb(b, sel_cb, LV_EVENT_CLICKED, r);
        } else {
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        }
        /* Verbindungsstrich zur Leiter */
        lv_obj_t *ln = box(r->ladder);
        lv_obj_set_size(ln, ui_px(10), 2);
        lv_obj_set_style_bg_color(ln, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        lv_obj_set_pos(ln, it[i].left ? barx - barw - ui_px(10) : barx + barw / 2, y);
    }
    #undef YPOS

    char buf[24];
    fmtv(r->fmt, it[r->sel].v, buf, sizeof(buf));
    lv_label_set_text_fmt(r->sel_name, "%s %s", it[r->sel].name, it[r->sel].cool ? "Kühlen" : "Heizen");
    lv_label_set_text(r->sel_val, buf);
}

static void open_settings(lv_event_t *e)
{
    ui_irc_t *r = lv_event_get_user_data(e);
    lv_obj_add_flag(r->main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(r->settings, LV_OBJ_FLAG_HIDDEN);
    r->dt_hash = 0;                         // Hash wird hier für die Leiter verwendet
    ladder_update(r);
}

static void close_settings(lv_event_t *e)
{
    ui_irc_t *r = lv_event_get_user_data(e);
    lv_obj_add_flag(r->settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(r->main, LV_OBJ_FLAG_HIDDEN);
    r->dt_hash = 0;                         // Zeitplan neu zeichnen
}

ui_irc_t *ui_irc_detail_create(lv_obj_t *parent, const cJSON *ctrl, const char *room)
{
    ui_irc_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM);
    if (r == NULL) return NULL;
    r->detail = true;
    load(r, ctrl);
    build_main(r, parent, room);
    build_settings(r, parent);
    ui_irc_update(r);
    return r;
}

void ui_irc_update(ui_irc_t *r)
{
    if (r == NULL) return;
    if (!r->detail) {
        tile_update(r);
        return;
    }
    if (!lv_obj_has_flag(r->main, LV_OBJ_FLAG_HIDDEN)) detail_update(r);
    else ladder_update(r);
}

void ui_irc_free(ui_irc_t *r)
{
    heap_caps_free(r);
}

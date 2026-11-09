/*
 * Energieflussmonitor (Loxone-Typ "EFM").
 *
 * Laut Loxone "Structure File":
 *   details.nodes: Baum aus Knoten {uuid, nodeType (Grid/Storage/Production/Load/Wallbox/Group),
 *     title, icon, [actualEfmState], [ctrlUuid], [nodes], [rest]}.
 *   Vorzeichen "actual": Grid > 0 Bezug, Storage > 0 Entladen, Production > 0 Erzeugung,
 *     Load/Wallbox > 0 Verbrauch.
 *   Status: Ppwr, Gpwr, Spwr, CO2 (kg/kWh), actual0..N (Live-Wert der Wurzelknoten).
 *   Befehle: getNodeValue/{view}/{uuid;uuid} -> {"uuid": {"actual", "storage", "total", "totalNeg"}}
 *            get/{view} -> actual: Gpwr, Ppwr, Spwr, Cpwr, Rest;
 *                          sonst: Export, Import, Production, Charge, Discharge, Consumption, Income
 *   view: actual, day, week, month, year, lifetime.
 */

#include "ui_efm.h"
#include "ui_priv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lox_client.h"
#include "lox_struct.h"

static const char *TAG = "ui_efm";

#define MAX_NODES   24
#define MAX_VIS     10
#define CH_SER      3       // Diagrammreihen
#define CH_MAX      31      // Balken

enum { NT_GRID, NT_STORAGE, NT_PROD, NT_LOAD, NT_WALLBOX, NT_GROUP };
enum { V_ACTUAL, V_DAY, V_WEEK, V_MONTH, V_YEAR, V_LIFE, V_COUNT };
static const char *const VIEW_CMD[V_COUNT] = { "actual", "day", "week", "month", "year", "lifetime" };
static const char *const VIEW_NAME[V_COUNT] = { "Leistung", "Tag", "Woche", "Monat", "Jahr", "Gesamt" };
static const char *const PERIOD_NAME[V_COUNT] = { "Heute", "Heute", "Diese Woche", "Dieser Monat",
                                                  "Dieses Jahr", "Seit Inbetriebnahme" };

enum { S_CONS, S_IMPORT, S_EXPORT, S_PROD, S_INCOME, S_CO2, S_COUNT };
static const char *const CARD_NAME[S_COUNT] = { "Verbrauch", "Netzbezug", "Einspeisung",
                                                "Energie erzeugt", "Geld gespart", "CO2 gespart" };

enum { IC_PYLON, IC_SUN, IC_CAR, IC_FLAME, IC_HOME, IC_BATT, IC_PLUG };

typedef struct {
    lox_uuid_t u;
    bool ok;
} st_t;

typedef struct {
    char uuid[40];
    char title[40];
    char icon[48];
    int type;
    int parent;             // -1 = Wurzel
    bool rest;
    st_t live;              // actualEfmState (nur Wurzelknoten)
    double v, vneg, storage;
    bool v_ok, storage_ok;
} node_t;

typedef struct {
    int node;               // Knotenindex oder -2 = Rest
    lv_obj_t *circle, *icon_box, *title, *val, *line, *dot;
    lv_point_precise_t pts[2];  // [0] am Zentrum, [1] am Knoten
    int dir;                // +1 zum Zentrum, -1 vom Zentrum weg, 0 kein Fluss
    int icon_kind;
    int soc_drawn;          // gezeichnete Akku-Stufe + 1 (0 = keine)
} vis_t;

typedef struct {
    bool ok;
    char uuid[40];
    char gid[8];
    char output[40];
} ch_ser_t;

struct ui_efm {
    bool detail;
    char uuid[48], name[64], room[48], currency[12];
    st_t ppwr, gpwr, spwr, co2;
    int nn;
    node_t nodes[MAX_NODES];
    /* Kachel */
    lv_obj_t *t_p, *t_v;
    /* Detail */
    int view, focus;
    bool rebuild;
    lv_obj_t *tabs[V_COUNT], *diagram, *period, *card_val[S_COUNT];
    lv_obj_t *hub, *hub_icon, *hub_title, *hub_val;
    int hub_icon_kind;
    int dw, dh;
    vis_t vis[MAX_VIS];
    int nvis;
    uint32_t gen;
    int64_t next_poll;
    double sum[S_COUNT];
    bool sum_ok[S_COUNT];
    double cpwr;
    bool cpwr_ok;
    /* Produktionsdiagramm */
    ch_ser_t ser[CH_SER];
    bool show_chart;
    lv_obj_t *ch_box, *chart, *ch_title, *leg_val[CH_SER], *toggle_lbl;
    double ch_val[CH_SER][CH_MAX];
    bool ch_has[CH_SER];
    int ch_n, ch_step, ch_year0;
    char ch_unit[8];
    time_t ch_from, ch_to;
    uint32_t ch_gen;
    bool ch_busy;
    int64_t ch_sent_us, ch_next_us;
};

/* ---------------- Antworten vom Miniserver ---------------- */

enum { Q_NODES, Q_SUM, Q_LIVE, Q_COUNT };
static SemaphoreHandle_t s_mx;
static char *s_res[Q_COUNT];
static uint32_t s_res_gen[Q_COUNT];
static uint32_t s_gen_counter;

/* läuft im WebSocket-Task */
static void query_cb(void *ctx, int code, const char *value)
{
    uintptr_t c = (uintptr_t)ctx;
    int kind = (int)(c & 3);
    uint32_t gen = (uint32_t)(c >> 2);
    if (code != 200 || value == NULL || kind >= Q_COUNT) {
        if (code) ESP_LOGW(TAG, "Abfrage %d: Code %d", kind, code);
        return;
    }
    size_t n = strlen(value);
    char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (copy == NULL) return;
    memcpy(copy, value, n + 1);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    heap_caps_free(s_res[kind]);
    s_res[kind] = copy;
    s_res_gen[kind] = gen;
    xSemaphoreGive(s_mx);
}

static cJSON *take_result(int kind, uint32_t gen)
{
    char *r = NULL;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_res[kind]) {
        if (s_res_gen[kind] == gen) r = s_res[kind];
        else heap_caps_free(s_res[kind]);
        s_res[kind] = NULL;
    }
    xSemaphoreGive(s_mx);
    if (r == NULL) return NULL;
    cJSON *j = cJSON_Parse(r);
    if (j == NULL) ESP_LOGW(TAG, "Antwort nicht lesbar: %.120s", r);
    heap_caps_free(r);
    return j;
}

static void ask(ui_efm_t *f, int kind, const char *cmd)
{
    void *ctx = (void *)(uintptr_t)(((uintptr_t)f->gen << 2) | (uintptr_t)kind);
    if (lox_query(f->uuid, cmd, query_cb, ctx) != ESP_OK) {
        f->next_poll = esp_timer_get_time() + 2000000;     // bald wieder versuchen
    }
}

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

/* Zahl aus JSON (Zahl oder Zeichenkette) */
static bool num(const cJSON *o, const char *k, double *out)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(v)) {
        *out = v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v)) {
        char *e;
        double d = strtod(v->valuestring, &e);
        if (e != v->valuestring) {
            *out = d;
            return true;
        }
    }
    return false;
}

static void comma(char *s)
{
    for (; *s; s++) {
        if (*s == '.') *s = ',';
    }
}

static void fmt_power(double kw, char *o, size_t n)
{
    double a = fabs(kw);
    if (a < 0.0005) snprintf(o, n, "0 W");
    else if (a < 0.9995) snprintf(o, n, "%.0f W", kw * 1000);
    else if (a < 99.95) snprintf(o, n, "%.1f kW", kw);
    else snprintf(o, n, "%.0f kW", kw);
    comma(o);
}

static void fmt_energy(double kwh, char *o, size_t n)
{
    double a = fabs(kwh);
    if (a < 0.0005) snprintf(o, n, "0 kWh");
    else if (a < 9.995) snprintf(o, n, "%.2f kWh", kwh);
    else if (a < 999.5) snprintf(o, n, "%.1f kWh", kwh);
    else if (a < 9999.5) snprintf(o, n, "%.0f kWh", kwh);
    else snprintf(o, n, "%.1f MWh", kwh / 1000);
    comma(o);
}

static int type_of(const char *t)
{
    if (t == NULL) return NT_LOAD;
    if (!strcmp(t, "Grid")) return NT_GRID;
    if (!strcmp(t, "Storage")) return NT_STORAGE;
    if (!strcmp(t, "Production")) return NT_PROD;
    if (!strcmp(t, "Wallbox")) return NT_WALLBOX;
    if (!strcmp(t, "Group")) return NT_GROUP;
    return NT_LOAD;
}

static bool is_source(int type)
{
    return type == NT_GRID || type == NT_STORAGE || type == NT_PROD;
}

static uint32_t type_color(int type)
{
    switch (type) {
    case NT_GRID:    return 0xa7b0bc;
    case NT_STORAGE: return 0x69c350;
    case NT_PROD:    return 0xf5c518;
    default:         return 0x4aa3df;
    }
}

static int icon_kind(const node_t *d)
{
    const char *p = d->icon;
    if (strcasestr(p, "grid") || (d->type == NT_GRID && !p[0])) return IC_PYLON;
    if (strcasestr(p, "solar") || strcasestr(p, "sun") || strcasestr(p, "pv")) return IC_SUN;
    if (strcasestr(p, "batter")) return IC_BATT;
    if (strcasestr(p, "car")) return IC_CAR;
    if (strcasestr(p, "heat") || strcasestr(p, "flame") || strcasestr(p, "fire")) return IC_FLAME;
    if (strcasestr(p, "home") || strcasestr(p, "house")) return IC_HOME;
    switch (d->type) {
    case NT_GRID:    return IC_PYLON;
    case NT_PROD:    return IC_SUN;
    case NT_STORAGE: return IC_BATT;
    case NT_WALLBOX: return IC_CAR;
    default:         return IC_PLUG;
    }
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

/* Symbol in einem Container (Grösse s) zeichnen; Akku-Symbol nach Ladestand */
static void draw_icon(lv_obj_t *holder, int kind, int s, lv_color_t c, double soc)
{
    lv_obj_clean(holder);
    lv_obj_t *o = NULL;
    switch (kind) {
    case IC_PYLON: o = ui_icon_pylon(holder, s, c); break;
    case IC_SUN:   o = ui_icon_sun(holder, s, c); break;
    case IC_CAR:   o = ui_icon_car(holder, s, c); break;
    case IC_FLAME: o = ui_icon_flame(holder, s, c); break;
    default: {
        const char *sym = kind == IC_HOME ? LV_SYMBOL_HOME : LV_SYMBOL_CHARGE;
        if (kind == IC_BATT) {
            sym = soc >= 85 ? LV_SYMBOL_BATTERY_FULL : soc >= 60 ? LV_SYMBOL_BATTERY_3
                : soc >= 35 ? LV_SYMBOL_BATTERY_2 : soc >= 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        }
        o = lv_label_create(holder);
        lv_label_set_text(o, sym);
        lv_obj_set_style_text_color(o, c, 0);
        lv_obj_set_style_text_font(o, s >= ui_px(40) ? &lv_font_montserrat_40 : &lv_font_montserrat_28, 0);
        break;
    }
    }
    if (o) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(o);
    }
}

/* ---------------- Laden ---------------- */

static void load_nodes(ui_efm_t *f, const cJSON *arr, int parent)
{
    const cJSON *n;
    cJSON_ArrayForEach(n, arr) {
        if (f->nn >= MAX_NODES) return;
        int i = f->nn++;
        node_t *d = &f->nodes[i];
        const cJSON *u = cJSON_GetObjectItem(n, "uuid");
        const cJSON *t = cJSON_GetObjectItem(n, "title");
        const cJSON *ic = cJSON_GetObjectItem(n, "icon");
        const cJSON *nt = cJSON_GetObjectItem(n, "nodeType");
        const cJSON *es = cJSON_GetObjectItem(n, "actualEfmState");
        strlcpy(d->uuid, cJSON_IsString(u) ? u->valuestring : "", sizeof(d->uuid));
        strlcpy(d->title, cJSON_IsString(t) ? t->valuestring : "?", sizeof(d->title));
        strlcpy(d->icon, cJSON_IsString(ic) ? ic->valuestring : "", sizeof(d->icon));
        d->type = type_of(cJSON_IsString(nt) ? nt->valuestring : NULL);
        d->parent = parent;
        d->rest = cJSON_IsTrue(cJSON_GetObjectItem(n, "rest"));
        d->live.ok = cJSON_IsString(es) && lox_uuid_parse(es->valuestring, &d->live.u);
        load_nodes(f, cJSON_GetObjectItem(n, "nodes"), i);
    }
}

static void load(ui_efm_t *f, const cJSON *ctrl, const char *room)
{
    const cJSON *u = cJSON_GetObjectItem(ctrl, "uuidAction");
    const cJSON *n = cJSON_GetObjectItem(ctrl, "name");
    strlcpy(f->uuid, cJSON_IsString(u) ? u->valuestring : "", sizeof(f->uuid));
    strlcpy(f->name, cJSON_IsString(n) ? n->valuestring : "Energiefluss", sizeof(f->name));
    strlcpy(f->room, room ? room : "", sizeof(f->room));
    st(ctrl, "Ppwr", &f->ppwr);
    st(ctrl, "Gpwr", &f->gpwr);
    st(ctrl, "Spwr", &f->spwr);
    st(ctrl, "CO2", &f->co2);
    const cJSON *cur = cJSON_GetObjectItem(cJSON_GetObjectItem(lox_struct_root(), "msInfo"), "currency");
    strlcpy(f->currency, cJSON_IsString(cur) ? cur->valuestring : "", sizeof(f->currency));
    load_nodes(f, cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "details"), "nodes"), -1);
}

/* Verbrauch aus den Live-Status: Erzeugung + Netzbezug + Speicher-Entladung */
static bool live_consumption(ui_efm_t *f, double *out)
{
    double p = 0, g = 0, s = 0;
    bool ok = val(&f->ppwr, &p);
    ok |= val(&f->gpwr, &g);
    val(&f->spwr, &s);
    *out = p + g + s;
    return ok;
}

/* ---------------- Kachel ---------------- */

static lv_obj_t *tile_row(lv_obj_t *tile, const char *letter, bool big)
{
    lv_obj_t *row = box(tile);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, ui_px(10), 0);
    lv_obj_t *l = label(row, big ? UI_F_M : UI_F_S, UI_COL_DIM, letter);
    lv_obj_set_style_pad_bottom(l, ui_px(big ? 6 : 3), 0);
    return label(row, big ? UI_F_XL : UI_F_L, UI_COL_TEXT, "-");
}

ui_efm_t *ui_efm_tile_create(lv_obj_t *tile, const cJSON *ctrl, const char *title, const char *room, int w, int h)
{
    ui_efm_t *f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_SPIRAM);
    if (f == NULL) return NULL;
    load(f, ctrl, room);
    const bool big = h >= 2;

    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tile, ui_px(12), 0);
    lv_obj_set_style_pad_row(tile, ui_px(2), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1e2126), 0);

    f->t_p = tile_row(tile, "P", big);
    f->t_v = tile_row(tile, "V", big);

    lv_obj_t *sp = box(tile);
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t *nm = label(tile, UI_F_S, UI_COL_TEXT, title && title[0] ? title : f->name);
    lv_obj_set_size(nm, LV_PCT(100), ui_px(40));      // höchstens zwei Zeilen
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_t *rm = label(tile, UI_F_S, UI_COL_DIM, f->room);
    lv_obj_set_width(rm, LV_PCT(100));
    lv_label_set_long_mode(rm, LV_LABEL_LONG_DOT);
    (void)w;

    ui_efm_update(f);
    return f;
}

static void tile_update(ui_efm_t *f)
{
    char b[24];
    double v;
    if (val(&f->ppwr, &v)) {
        fmt_power(v, b, sizeof(b));
        lv_label_set_text(f->t_p, b);
    }
    if (live_consumption(f, &v)) {
        fmt_power(v, b, sizeof(b));
        lv_label_set_text(f->t_v, b);
    }
}

/* ---------------- Detail: Flussdiagramm ---------------- */

static void dot_exec(void *var, int32_t v)
{
    vis_t *vs = var;
    float t = v / 1000.0f;
    if (vs->dir < 0) t = 1.0f - t;
    /* dir > 0: vom Knoten [1] zum Zentrum [0] */
    float x = vs->pts[1].x + (vs->pts[0].x - vs->pts[1].x) * t;
    float y = vs->pts[1].y + (vs->pts[0].y - vs->pts[1].y) * t;
    int32_t s = lv_obj_get_width(vs->dot);
    lv_obj_set_pos(vs->dot, (int32_t)x - s / 2, (int32_t)y - s / 2);
}

static void set_flow(vis_t *vs, int dir)
{
    if (vs->dot == NULL || dir == vs->dir) return;
    vs->dir = dir;
    lv_anim_delete(vs, dot_exec);
    if (dir == 0) {
        lv_obj_add_flag(vs->dot, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(vs->dot, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, vs);
    lv_anim_set_exec_cb(&a, dot_exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 1600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void stop_anims(ui_efm_t *f)
{
    for (int i = 0; i < f->nvis; i++) {
        lv_anim_delete(&f->vis[i], dot_exec);
        f->vis[i].dir = 0;
    }
}

static void restart_queries(ui_efm_t *f)
{
    f->gen = ++s_gen_counter & 0x3fffffff;
    f->next_poll = 0;
    for (int i = 0; i < f->nn; i++) {
        f->nodes[i].v_ok = false;
        f->nodes[i].storage_ok = false;
    }
    for (int i = 0; i < S_COUNT; i++) f->sum_ok[i] = false;
    f->cpwr_ok = false;
}

static bool has_children(ui_efm_t *f, int idx)
{
    for (int i = 0; i < f->nn; i++) {
        if (f->nodes[i].parent == idx) return true;
    }
    return false;
}

static void node_cb(lv_event_t *e)
{
    ui_efm_t *f = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (idx >= 0 && idx < f->nn && has_children(f, idx)) {
        f->focus = idx;
        f->rebuild = true;          // nicht im Ereignis des gelöschten Objekts neu aufbauen
    }
}

static void hub_cb(lv_event_t *e)
{
    ui_efm_t *f = lv_event_get_user_data(e);
    if (f->focus >= 0) {
        f->focus = f->nodes[f->focus].parent;
        f->rebuild = true;
    }
}

static lv_obj_t *circle(lv_obj_t *parent, int r, uint32_t border)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, 2 * r, 2 * r);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x1e2126), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(c, ui_px(3), 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void build_diagram(ui_efm_t *f)
{
    stop_anims(f);
    lv_obj_clean(f->diagram);
    memset(f->vis, 0, sizeof(f->vis));
    f->nvis = 0;
    f->hub_icon_kind = -1;

    for (int i = 0; i < f->nn && f->nvis < MAX_VIS - 1; i++) {
        if (f->nodes[i].parent == f->focus) f->vis[f->nvis++].node = i;
    }
    if (f->focus >= 0 && f->nodes[f->focus].rest && f->nvis > 0) f->vis[f->nvis++].node = -2;

    /* Im Kreis um das Zentrum, gleiche Winkel (360°/n), im Uhrzeigersinn ab oben Mitte.
       Die Zuleitung (Netz) steht immer oben. */
    int order[MAX_VIS], no = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < f->nvis; i++) {
            int nd = f->vis[i].node;
            bool grid = nd >= 0 && f->nodes[nd].type == NT_GRID;
            if (grid == (pass == 0)) order[no++] = i;
        }
    }

    const int W = f->dw, H = f->dh;
    const int n = f->nvis;
    const int R = ui_px(56);
    const int r = n <= 6 ? ui_px(34) : n <= 8 ? ui_px(30) : ui_px(25);
    const int lbl_h = ui_px(40);                // Name + Wert
    const int lbl_w = n <= 6 ? ui_px(140) : ui_px(112);
    const float cx = W / 2.0f, cy = H / 2.0f;
    /* Beschriftung liegt aussen (oben über, unten unter dem Kreis) */
    float ry = H / 2.0f - r - lbl_h - ui_px(2);
    float rx = W / 2.0f - lbl_w / 2.0f - ui_px(4);
    if (ry < R + r) ry = R + r;
    const float step = n > 0 ? 2.0f * (float)M_PI / n : 0;

    float px[MAX_VIS], py[MAX_VIS];
    bool lbl_above[MAX_VIS];
    for (int k = 0; k < n; k++) {
        int i = order[k];
        float an = -(float)M_PI / 2.0f + k * step;
        px[i] = cx + rx * cosf(an);
        py[i] = cy + ry * sinf(an);
        lbl_above[i] = sinf(an) < -0.3f;
    }

    /* Linien (unter den Kreisen) */
    for (int i = 0; i < f->nvis; i++) {
        vis_t *vs = &f->vis[i];
        float dx = px[i] - cx, dy = py[i] - cy;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1) len = 1;
        float ux = dx / len, uy = dy / len;
        vs->pts[0].x = cx + ux * R;
        vs->pts[0].y = cy + uy * R;
        vs->pts[1].x = px[i] - ux * r;
        vs->pts[1].y = py[i] - uy * r;
        vs->line = lv_line_create(f->diagram);
        lv_line_set_points(vs->line, vs->pts, 2);
        lv_obj_set_style_line_width(vs->line, ui_px(3), 0);
        lv_obj_set_style_line_color(vs->line, lv_color_hex(0x3a424d), 0);
        lv_obj_set_style_line_rounded(vs->line, true, 0);
    }

    /* Zentrum */
    const node_t *fn = f->focus >= 0 ? &f->nodes[f->focus] : NULL;
    f->hub = circle(f->diagram, R, fn ? type_color(fn->type) : UI_COL_ON);
    lv_obj_set_pos(f->hub, (int32_t)(cx - R), (int32_t)(cy - R));
    lv_obj_add_event_cb(f->hub, hub_cb, LV_EVENT_CLICKED, f);
    f->hub_icon = box(f->hub);
    lv_obj_set_size(f->hub_icon, ui_px(36), ui_px(36));
    lv_obj_align(f->hub_icon, LV_ALIGN_TOP_MID, 0, ui_px(10));
    lv_obj_remove_flag(f->hub_icon, LV_OBJ_FLAG_CLICKABLE);
    f->hub_val = label(f->hub, UI_F_M, UI_COL_TEXT, "-");
    lv_obj_align(f->hub_val, LV_ALIGN_TOP_MID, 0, ui_px(46));
    f->hub_title = label(f->hub, UI_F_S, UI_COL_DIM, fn ? fn->title : "Verbrauch");
    lv_obj_set_size(f->hub_title, 2 * R - ui_px(16), ui_px(20));
    lv_obj_set_style_text_align(f->hub_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(f->hub_title, LV_LABEL_LONG_DOT);
    lv_obj_align(f->hub_title, LV_ALIGN_TOP_MID, 0, ui_px(70));
    draw_icon(f->hub_icon, fn ? icon_kind(fn) : IC_HOME, ui_px(36),
              lv_color_hex(fn ? type_color(fn->type) : UI_COL_ON), 50);
    f->hub_icon_kind = fn ? icon_kind(fn) : IC_HOME;

    /* Knoten */
    for (int i = 0; i < f->nvis; i++) {
        vis_t *vs = &f->vis[i];
        const node_t *d = vs->node >= 0 ? &f->nodes[vs->node] : NULL;
        uint32_t col = d ? type_color(d->type) : UI_COL_DIM;
        vs->circle = circle(f->diagram, r, col);
        lv_obj_set_pos(vs->circle, (int32_t)px[i] - r, (int32_t)py[i] - r);
        lv_obj_set_user_data(vs->circle, (void *)(intptr_t)vs->node);
        lv_obj_add_event_cb(vs->circle, node_cb, LV_EVENT_CLICKED, f);
        if (d && has_children(f, vs->node)) {
            lv_obj_set_style_shadow_color(vs->circle, lv_color_hex(col), 0);
            lv_obj_set_style_shadow_width(vs->circle, ui_px(14), 0);
            lv_obj_set_style_shadow_opa(vs->circle, LV_OPA_40, 0);
        }
        int is = r * 5 / 4;
        vs->icon_box = box(vs->circle);
        lv_obj_set_size(vs->icon_box, is, is);
        lv_obj_center(vs->icon_box);
        lv_obj_remove_flag(vs->icon_box, LV_OBJ_FLAG_CLICKABLE);
        vs->icon_kind = d ? icon_kind(d) : IC_PLUG;
        draw_icon(vs->icon_box, vs->icon_kind, is, lv_color_hex(col), 50);

        vs->title = label(f->diagram, UI_F_S, UI_COL_DIM, d ? d->title : "Rest");
        lv_obj_set_size(vs->title, lbl_w, ui_px(20));     // eine Zeile, sonst "..."
        lv_obj_set_style_text_align(vs->title, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(vs->title, LV_LABEL_LONG_DOT);
        int ly = lbl_above[i] ? (int)py[i] - r - lbl_h : (int)py[i] + r + ui_px(2);
        lv_obj_set_pos(vs->title, (int32_t)px[i] - lbl_w / 2, ly);
        vs->val = label(f->diagram, UI_F_S, UI_COL_TEXT, "-");
        lv_obj_set_size(vs->val, lbl_w, ui_px(20));
        lv_label_set_long_mode(vs->val, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(vs->val, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(vs->val, (int32_t)px[i] - lbl_w / 2, ly + ui_px(19));

        vs->dot = box(f->diagram);
        lv_obj_set_size(vs->dot, ui_px(12), ui_px(12));
        lv_obj_set_style_radius(vs->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(vs->dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(vs->dot, lv_color_hex(col), 0);
        lv_obj_add_flag(vs->dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(vs->dot, LV_OBJ_FLAG_CLICKABLE);
        vs->dir = 0;
    }

    if (f->focus >= 0) {
        lv_obj_t *back = lv_button_create(f->diagram);
        lv_obj_set_style_bg_color(back, lv_color_hex(UI_COL_BTN), 0);
        lv_obj_set_style_shadow_width(back, 0, 0);
        lv_obj_set_pos(back, 0, 0);
        lv_obj_t *bl = label(back, UI_F_S, UI_COL_TEXT, LV_SYMBOL_LEFT " Zurück");
        lv_obj_center(bl);
        lv_obj_add_event_cb(back, hub_cb, LV_EVENT_CLICKED, f);
    }
}

/* ---------------- Detail: Produktionsdiagramm (StatisticV2) ---------------- */

/* Reihenfarben wie in der App: Eigenverbrauch, Speicher, Netz-Einspeisung */
static const uint32_t SER_COL[CH_SER] = { 0x69c350, 0x4aa3df, 0xa7b0bc };
static const char *const SER_NAME[CH_SER] = { "Eigenverbrauch", "Speicher", "Netz-Einspeisung" };

/* Statistikgruppe eines Bausteins mit der gesuchten Ausgabe finden */
static bool find_stat(const cJSON *ctrl, const char *output, char *gid, size_t gsize)
{
    const cJSON *g;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "statisticV2"), "groups")) {
        const cJSON *dp;
        cJSON_ArrayForEach(dp, cJSON_GetObjectItem(g, "dataPoints")) {
            const cJSON *o = cJSON_GetObjectItem(dp, "output");
            if (cJSON_IsString(o) && !strcmp(o->valuestring, output)) {
                const cJSON *id = cJSON_GetObjectItem(g, "id");
                if (cJSON_IsString(id)) strlcpy(gid, id->valuestring, gsize);
                else if (cJSON_IsNumber(id)) snprintf(gid, gsize, "%d", id->valueint);
                else return false;
                return true;
            }
        }
    }
    return false;
}

/* unter lox_struct_lock */
static void chart_load(ui_efm_t *f, const cJSON *ctrl)
{
    static const char *const efm_out[2] = { "totalSelfConsumption", "totalStorageChargeFromProduction" };
    for (int s = 0; s < 2; s++) {
        ch_ser_t *c = &f->ser[s];
        if (find_stat(ctrl, efm_out[s], c->gid, sizeof(c->gid))) {
            strlcpy(c->uuid, f->uuid, sizeof(c->uuid));
            strlcpy(c->output, efm_out[s], sizeof(c->output));
            c->ok = true;
        }
    }
    /* Netz-Einspeisung: Zähler des Netzknotens (Wurzel), Ausgabe totalNeg */
    const cJSON *n;
    cJSON_ArrayForEach(n, cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "details"), "nodes")) {
        const cJSON *nt = cJSON_GetObjectItem(n, "nodeType");
        const cJSON *cu = cJSON_GetObjectItem(n, "ctrlUuid");
        if (!cJSON_IsString(nt) || strcmp(nt->valuestring, "Grid") || !cJSON_IsString(cu)) continue;
        const cJSON *meter = lox_struct_find_control(cu->valuestring, NULL);
        ch_ser_t *c = &f->ser[2];
        if (meter && find_stat(meter, "totalNeg", c->gid, sizeof(c->gid))) {
            strlcpy(c->uuid, cu->valuestring, sizeof(c->uuid));
            strlcpy(c->output, "totalNeg", sizeof(c->output));
            c->ok = true;
        }
        break;
    }
}

/* Zeitraum und Einteilung für die Ansicht (Ortszeit) */
static void chart_period(ui_efm_t *f)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    t.tm_isdst = -1;
    struct tm e = t;
    switch (f->view) {
    case V_WEEK: {
        int wd = (t.tm_wday + 6) % 7;           // Montag = 0
        t.tm_mday -= wd;
        e = t;
        e.tm_mday += 7;
        strlcpy(f->ch_unit, "day", sizeof(f->ch_unit));
        f->ch_n = 7;
        break;
    }
    case V_MONTH:
        t.tm_mday = 1;
        e = t;
        e.tm_mon += 1;
        strlcpy(f->ch_unit, "day", sizeof(f->ch_unit));
        break;
    case V_YEAR:
        t.tm_mday = 1;
        t.tm_mon = 0;
        e = t;
        e.tm_year += 1;
        strlcpy(f->ch_unit, "month", sizeof(f->ch_unit));
        f->ch_n = 12;
        break;
    case V_LIFE:
        t.tm_mday = 1;
        t.tm_mon = 0;
        e = t;
        e.tm_year += 1;
        t.tm_year -= 9;                         // letzte 10 Jahre
        strlcpy(f->ch_unit, "year", sizeof(f->ch_unit));
        f->ch_n = 10;
        break;
    default:                                    // Leistung, Tag: heute stündlich
        e.tm_mday += 1;
        strlcpy(f->ch_unit, "hour", sizeof(f->ch_unit));
        f->ch_n = 24;
        break;
    }
    f->ch_from = mktime(&t);
    f->ch_to = mktime(&e) - 1;
    f->ch_year0 = t.tm_year;
    if (f->view == V_MONTH) f->ch_n = (int)((f->ch_to + 1 - f->ch_from + 3600) / 86400);
    if (f->ch_n > CH_MAX) f->ch_n = CH_MAX;
}

/* Balkennummer eines Zeitstempels */
static int chart_index(ui_efm_t *f, time_t ts)
{
    struct tm t;
    localtime_r(&ts, &t);
    switch (f->view) {
    case V_WEEK:  return (t.tm_wday + 6) % 7;
    case V_MONTH: return t.tm_mday - 1;
    case V_YEAR:  return t.tm_mon;
    case V_LIFE:  return t.tm_year - f->ch_year0;
    default:      return t.tm_hour;
    }
}

/* Ergebnis der binären Abfrage (kommt aus dem WebSocket-Task) */
static uint8_t *s_bin;
static size_t s_bin_len;
static uint32_t s_bin_tag;
static int s_bin_code;
static bool s_bin_done;

static void bin_cb(void *ctx, int code, const uint8_t *data, size_t len)
{
    uint8_t *copy = NULL;
    if (code == 200 && len) {
        copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (copy) memcpy(copy, data, len);
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    heap_caps_free(s_bin);
    s_bin = copy;
    s_bin_len = copy ? len : 0;
    s_bin_tag = (uint32_t)(uintptr_t)ctx;
    s_bin_code = code;
    s_bin_done = true;
    xSemaphoreGive(s_mx);
}

static uint32_t chart_tag(ui_efm_t *f)
{
    return (f->ch_gen << 2) | (uint32_t)f->ch_step;
}

/* nächste Reihe abfragen; true = Anfrage läuft */
static void chart_ask(ui_efm_t *f)
{
    while (f->ch_step < CH_SER && !f->ser[f->ch_step].ok) f->ch_step++;
    if (f->ch_step >= CH_SER) {
        f->ch_busy = false;
        f->ch_next_us = esp_timer_get_time() + 300000000LL;        // alle 5 min neu
        lv_obj_invalidate(f->chart);
        return;
    }
    const ch_ser_t *c = &f->ser[f->ch_step];
    char path[200];
    snprintf(path, sizeof(path), "jdev/sps/getStatistic/%s/diff/%lld/%lld/%s/%s/%s", c->uuid,
             (long long)f->ch_from, (long long)f->ch_to, f->ch_unit, c->gid, c->output);
    if (lox_query_bin(path, bin_cb, (void *)(uintptr_t)chart_tag(f)) == ESP_OK) {
        f->ch_busy = true;
        f->ch_sent_us = esp_timer_get_time();
    } else {
        f->ch_busy = false;
        f->ch_next_us = esp_timer_get_time() + 2000000;            // gleich nochmals
    }
}

static void chart_restart(ui_efm_t *f)
{
    f->ch_gen = (f->ch_gen + 1) & 0x3fffffff;
    memset(f->ch_val, 0, sizeof(f->ch_val));
    memset(f->ch_has, 0, sizeof(f->ch_has));
    chart_period(f);
    f->ch_step = 0;
    f->ch_busy = false;
    f->ch_next_us = 0;
    lv_obj_invalidate(f->chart);
}

static void chart_poll(ui_efm_t *f)
{
    int64_t now = esp_timer_get_time();
    uint8_t *data = NULL;
    size_t len = 0;
    bool done = false;
    int code = 0;
    uint32_t tag = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_bin_done) {
        done = true;
        data = s_bin;
        len = s_bin_len;
        tag = s_bin_tag;
        code = s_bin_code;
        s_bin = NULL;
        s_bin_done = false;
    }
    xSemaphoreGive(s_mx);

    if (done && f->ch_busy && tag == chart_tag(f)) {
        int s = f->ch_step;
        if (code == 200) {
            /* Einträge: Uint32 Zeit (UTC) + Float64 Wert */
            for (size_t o = 0; o + 12 <= len; o += 12) {
                uint32_t ts;
                double v;
                memcpy(&ts, data + o, 4);
                memcpy(&v, data + o + 4, 8);
                int i = chart_index(f, (time_t)ts);
                if (i >= 0 && i < f->ch_n && isfinite(v) && v > 0) f->ch_val[s][i] += v;
            }
            f->ch_has[s] = true;
            ESP_LOGI(TAG, "Statistik %s: %u Einträge", SER_NAME[s], (unsigned)(len / 12));
        } else {
            ESP_LOGW(TAG, "Statistik %s: Code %d", SER_NAME[s], code);
        }
        f->ch_step++;
        f->ch_busy = false;
        lv_obj_invalidate(f->chart);
        chart_ask(f);
    }
    heap_caps_free(data);

    if (f->ch_busy && now - f->ch_sent_us > 20000000) {        // keine Antwort: Reihe überspringen
        f->ch_step++;
        f->ch_busy = false;
        chart_ask(f);
    }
    if (!f->ch_busy && now >= f->ch_next_us) {
        if (f->ch_step >= CH_SER) chart_restart(f);
        chart_ask(f);
    }
}

static const char *const MON_ABBR[12] = { "Jan", "Feb", "Mär", "Apr", "Mai", "Jun",
                                          "Jul", "Aug", "Sep", "Okt", "Nov", "Dez" };
static const char *const DAY_ABBR[7] = { "Mo", "Di", "Mi", "Do", "Fr", "Sa", "So" };

static void chart_draw(lv_event_t *e)
{
    ui_efm_t *f = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_content_coords(o, &a);
    const int lab_h = 18;
    const int x0 = a.x1 + ui_px(70), x1 = a.x2, y0 = a.y1 + 8, y1 = a.y2 - lab_h - 4;
    if (x1 <= x0 + 10 || y1 <= y0 + 10 || f->ch_n <= 0) return;

    double max = 0;
    for (int i = 0; i < f->ch_n; i++) {
        double s = 0;
        for (int k = 0; k < CH_SER; k++) s += f->ch_val[k][i];
        if (s > max) max = s;
    }
    /* runde Obergrenze: 0,5 / 1 / 2 / 5 / 10 ... */
    double top = 0.5;
    static const double steps[] = { 1, 2, 5 };
    for (double dec = 1; top < max; dec *= 10) {
        for (int i = 0; i < 3 && top < max; i++) top = steps[i] * dec;
    }

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = lv_color_hex(0x3a424d);
    ld.width = 1;
    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.color = lv_color_hex(UI_COL_DIM);
    td.font = &lv_font_montserrat_14;
    char b[24];
    for (int g = 0; g <= 2; g++) {
        int y = y1 - (y1 - y0) * g / 2;
        ld.p1.x = x0; ld.p1.y = y; ld.p2.x = x1; ld.p2.y = y;
        lv_draw_line(layer, &ld);
        double v = top * g / 2;
        snprintf(b, sizeof(b), v < 10 && v != (int)v ? "%.1f kWh" : "%.0f kWh", v);
        comma(b);
        td.text = b;
        td.align = LV_TEXT_ALIGN_RIGHT;
        lv_area_t ta = { a.x1, y - 8, x0 - 6, y + 8 };
        lv_draw_label(layer, &td, &ta);
    }

    const float slot = (float)(x1 - x0) / f->ch_n;
    int bw = (int)(slot * 0.7f);
    if (bw < 2) bw = 2;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 2;
    for (int i = 0; i < f->ch_n; i++) {
        int bx = x0 + (int)(slot * i + (slot - bw) / 2);
        double acc = 0;
        for (int k = 0; k < CH_SER; k++) {
            double v = f->ch_val[k][i];
            if (v <= 0) continue;
            int ya = y1 - (int)((y1 - y0) * (acc / top));
            int yb = y1 - (int)((y1 - y0) * ((acc + v) / top));
            acc += v;
            if (ya - yb < 1) yb = ya - 1;
            rd.bg_color = lv_color_hex(SER_COL[k]);
            lv_area_t ra = { bx, yb, bx + bw - 1, ya };
            lv_draw_rect(layer, &rd, &ra);
        }
        /* Achsenbeschriftung, nicht für jeden Balken */
        bool show;
        if (f->view == V_MONTH) show = i == 0 || (i + 1) % 5 == 0;
        else if (f->view == V_ACTUAL || f->view == V_DAY) show = i % 6 == 0;
        else if (f->view == V_YEAR) show = i % 2 == 0;
        else show = true;
        if (!show) continue;
        if (f->view == V_WEEK) strlcpy(b, DAY_ABBR[i], sizeof(b));
        else if (f->view == V_MONTH) snprintf(b, sizeof(b), "%d.", i + 1);
        else if (f->view == V_YEAR) strlcpy(b, MON_ABBR[i], sizeof(b));
        else if (f->view == V_LIFE) snprintf(b, sizeof(b), "%d", 1900 + f->ch_year0 + i);
        else snprintf(b, sizeof(b), "%d:00", i);
        td.text = b;
        td.align = LV_TEXT_ALIGN_CENTER;
        int cx = x0 + (int)(slot * i + slot / 2);
        lv_area_t la = { cx - 30, y1 + 4, cx + 30, y1 + 4 + lab_h };
        lv_draw_label(layer, &td, &la);
    }
}

static void chart_legend_update(ui_efm_t *f)
{
    for (int k = 0; k < CH_SER; k++) {
        if (f->leg_val[k] == NULL) continue;
        double s = 0;
        for (int i = 0; i < f->ch_n; i++) s += f->ch_val[k][i];
        char b[48];
        if (!f->ch_has[k]) strlcpy(b, "...", sizeof(b));
        else fmt_energy(s, b, sizeof(b));
        if (strcmp(lv_label_get_text(f->leg_val[k]), b)) lv_label_set_text(f->leg_val[k], b);
    }
    static const char *const title[V_COUNT] = { "Produktion heute", "Produktion heute", "Produktion diese Woche",
                                                "Produktion dieser Monat", "Produktion dieses Jahr",
                                                "Produktion pro Jahr" };
    if (strcmp(lv_label_get_text(f->ch_title), title[f->view])) lv_label_set_text(f->ch_title, title[f->view]);
}

static void chart_create(ui_efm_t *f, lv_obj_t *parent, int x, int y, int w, int h)
{
    f->ch_box = box(parent);
    lv_obj_set_pos(f->ch_box, x, y);
    lv_obj_set_size(f->ch_box, w, h);
    lv_obj_add_flag(f->ch_box, LV_OBJ_FLAG_HIDDEN);

    f->ch_title = label(f->ch_box, UI_F_M, UI_COL_TEXT, "");
    lv_obj_set_pos(f->ch_title, 0, 0);

    const int leg_h = ui_px(48), btn_h = ui_px(48);
    f->chart = box(f->ch_box);
    lv_obj_set_pos(f->chart, 0, ui_px(36));
    lv_obj_set_size(f->chart, w, h - ui_px(36) - leg_h - btn_h);
    lv_obj_add_event_cb(f->chart, chart_draw, LV_EVENT_DRAW_MAIN_END, f);

    /* Legende mit Summen */
    lv_obj_t *leg = box(f->ch_box);
    lv_obj_set_size(leg, w, leg_h);
    lv_obj_set_pos(leg, 0, h - leg_h - btn_h);
    lv_obj_set_flex_flow(leg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(leg, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int k = 0; k < CH_SER; k++) {
        if (!f->ser[k].ok) continue;
        lv_obj_t *it = box(leg);
        lv_obj_set_size(it, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(it, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *r = box(it);
        lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, ui_px(6), 0);
        lv_obj_t *dot = box(r);
        lv_obj_set_size(dot, ui_px(12), ui_px(12));
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(SER_COL[k]), 0);
        label(r, UI_F_S, UI_COL_DIM, SER_NAME[k]);
        f->leg_val[k] = label(it, UI_F_S, UI_COL_TEXT, "...");
    }
    if (!f->ser[0].ok && !f->ser[1].ok && !f->ser[2].ok) {
        label(leg, UI_F_S, UI_COL_DIM, "Keine Statistik im Energieflussmonitor aktiviert");
    }
}

static void toggle_cb(lv_event_t *e)
{
    ui_efm_t *f = lv_event_get_user_data(e);
    f->show_chart = !f->show_chart;
    if (f->show_chart) {
        lv_obj_add_flag(f->diagram, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(f->ch_box, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(f->toggle_lbl, LV_SYMBOL_LOOP " Fluss");
        chart_restart(f);
    } else {
        lv_obj_remove_flag(f->diagram, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(f->ch_box, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(f->toggle_lbl, LV_SYMBOL_IMAGE " Verlauf");
    }
}

/* ---------------- Detail: Aufbau ---------------- */

static void style_tabs(ui_efm_t *f)
{
    for (int i = 0; i < V_COUNT; i++) {
        bool on = i == f->view;
        lv_obj_set_style_bg_color(f->tabs[i], lv_color_hex(on ? UI_COL_ON : 0x232a33), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(f->tabs[i], 0), lv_color_hex(on ? 0x111111 : UI_COL_TEXT), 0);
    }
    lv_label_set_text(f->period, PERIOD_NAME[f->view]);
}

static void tab_cb(lv_event_t *e)
{
    ui_efm_t *f = lv_event_get_user_data(e);
    int v = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (v == f->view || v < 0 || v >= V_COUNT) return;
    f->view = v;
    style_tabs(f);
    restart_queries(f);
    if (f->show_chart) chart_restart(f);
}

ui_efm_t *ui_efm_detail_create(lv_obj_t *parent, const cJSON *ctrl, const char *room)
{
    if (s_mx == NULL) s_mx = xSemaphoreCreateMutex();
    if (s_mx == NULL) return NULL;
    ui_efm_t *f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_SPIRAM);
    if (f == NULL) return NULL;
    load(f, ctrl, room);
    chart_load(f, ctrl);
    f->detail = true;
    f->focus = -1;
    f->view = V_ACTUAL;

    lv_obj_update_layout(parent);
    int W = lv_obj_get_content_width(parent);
    int H = lv_obj_get_content_height(parent);
    if (W < 200) W = 940;
    if (H < 200) H = 440;

    /* Titel + Ansichten */
    lv_obj_t *tl = label(parent, UI_F_M, UI_COL_TEXT, f->name);
    lv_obj_set_size(tl, W * 36 / 100, ui_px(30));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(tl, 0, ui_px(8));

    lv_obj_t *row = box(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, ui_px(6), 0);
    lv_obj_align(row, LV_ALIGN_TOP_RIGHT, -ui_px(58), 0);
    for (int i = 0; i < V_COUNT; i++) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_height(b, ui_px(44));
        lv_obj_set_style_pad_hor(b, ui_px(14), 0);
        lv_obj_set_style_radius(b, ui_px(22), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_t *l = label(b, UI_F_S, UI_COL_TEXT, VIEW_NAME[i]);
        lv_obj_center(l);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, f);
        f->tabs[i] = b;
    }

    const int top = ui_px(60);
    f->dw = W * 60 / 100;
    f->dh = H - top;
    f->diagram = box(parent);
    lv_obj_set_pos(f->diagram, 0, top);
    lv_obj_set_size(f->diagram, f->dw, f->dh);
    chart_create(f, parent, 0, top, f->dw, f->dh);
    lv_obj_t *tg = lv_button_create(parent);
    lv_obj_set_style_bg_color(tg, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_set_style_shadow_width(tg, 0, 0);
    lv_obj_set_style_radius(tg, ui_px(20), 0);
    lv_obj_set_height(tg, ui_px(40));
    f->toggle_lbl = label(tg, UI_F_S, UI_COL_TEXT, LV_SYMBOL_IMAGE " Verlauf");
    lv_obj_center(f->toggle_lbl);
    lv_obj_set_pos(tg, 0, top + f->dh - ui_px(40));
    lv_obj_add_event_cb(tg, toggle_cb, LV_EVENT_CLICKED, f);

    /* Kennzahlen rechts */
    const int rx = f->dw + ui_px(16), rw = W - rx;
    f->period = label(parent, UI_F_M, UI_COL_DIM, "");
    lv_obj_set_pos(f->period, rx, top);
    const int gap = ui_px(10), cw = (rw - gap) / 2, ch = ui_px(92);
    for (int i = 0; i < S_COUNT; i++) {
        lv_obj_t *c = lv_obj_create(parent);
        lv_obj_remove_style_all(c);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(c, cw, ch);
        lv_obj_set_pos(c, rx + (i % 2) * (cw + gap), top + ui_px(40) + (i / 2) * (ch + gap));
        lv_obj_set_style_bg_color(c, lv_color_hex(0x232a33), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, ui_px(12), 0);
        lv_obj_set_style_pad_all(c, ui_px(10), 0);
        lv_obj_t *t = label(c, UI_F_S, UI_COL_DIM, CARD_NAME[i]);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
        f->card_val[i] = label(c, UI_F_L, UI_COL_TEXT, "-");
        lv_obj_align(f->card_val[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    style_tabs(f);
    build_diagram(f);
    restart_queries(f);
    ui_efm_update(f);
    ESP_LOGI(TAG, "Detail offen: intern frei %u, DMA grösster Block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    return f;
}

/* ---------------- Detail: Werte ---------------- */

static void poll(ui_efm_t *f)
{
    int64_t now = esp_timer_get_time();
    if (now < f->next_poll) return;
    lox_status_t ls;
    lox_get_status(&ls);
    if (ls.state != LOX_ONLINE) {
        f->next_poll = now + 2000000;
        return;
    }
    f->next_poll = now + (f->view == V_ACTUAL ? 5000000LL : 60000000LL);

    /* Knotenwerte: sichtbare Knoten + Fokus */
    char cmd[640];
    int n = snprintf(cmd, sizeof(cmd), "getNodeValue/%s/", VIEW_CMD[f->view]);
    bool any = false;
    for (int i = -1; i < f->nvis; i++) {
        int nd = i < 0 ? f->focus : f->vis[i].node;
        if (nd < 0 || !f->nodes[nd].uuid[0]) continue;
        if (n + 42 >= (int)sizeof(cmd)) break;
        n += snprintf(cmd + n, sizeof(cmd) - n, "%s%s", any ? ";" : "", f->nodes[nd].uuid);
        any = true;
    }
    if (any) ask(f, Q_NODES, cmd);

    snprintf(cmd, sizeof(cmd), "get/%s", f->view == V_ACTUAL ? "day" : VIEW_CMD[f->view]);
    ask(f, Q_SUM, cmd);
    if (f->view == V_ACTUAL) ask(f, Q_LIVE, "get/actual");
}

static void take_results(ui_efm_t *f)
{
    cJSON *j = take_result(Q_NODES, f->gen);
    if (j) {
        for (int i = 0; i < f->nn; i++) {
            node_t *d = &f->nodes[i];
            const cJSON *o = cJSON_GetObjectItem(j, d->uuid);
            if (o == NULL) continue;
            double v;
            if (f->view == V_ACTUAL) {
                if (num(o, "actual", &v)) { d->v = v; d->v_ok = true; }
                if (num(o, "storage", &v)) { d->storage = v; d->storage_ok = true; }
            } else {
                if (num(o, "total", &v)) { d->v = v; d->v_ok = true; }
                d->vneg = num(o, "totalNeg", &v) ? v : 0;
            }
        }
        cJSON_Delete(j);
    }
    j = take_result(Q_SUM, f->gen);
    if (j) {
        static const char *const keys[] = { "Consumption", "Import", "Export", "Production", "Income" };
        for (int i = 0; i < 5; i++) f->sum_ok[i] = num(j, keys[i], &f->sum[i]);
        double co2 = 0;
        f->sum_ok[S_CO2] = f->sum_ok[S_PROD] && val(&f->co2, &co2);
        f->sum[S_CO2] = f->sum[S_PROD] * co2;
        cJSON_Delete(j);
    }
    j = take_result(Q_LIVE, f->gen);
    if (j) {
        f->cpwr_ok = num(j, "Cpwr", &f->cpwr);
        cJSON_Delete(j);
    }
}

/* Wert eines Knotens für die aktuelle Ansicht (Live-Status hat Vorrang) */
static bool node_value(ui_efm_t *f, int idx, double *v)
{
    node_t *d = &f->nodes[idx];
    if (f->view == V_ACTUAL && d->live.ok && lox_value_get(&d->live.u, v)) return true;
    *v = d->v;
    return d->v_ok;
}

static void value_text(ui_efm_t *f, int idx, double v, char *o, size_t n)
{
    char a[24], b[24];
    if (f->view == V_ACTUAL) {
        fmt_power(v, a, sizeof(a));
        if (idx >= 0 && f->nodes[idx].storage_ok) {
            snprintf(o, n, "%s  %.0f %%", a, f->nodes[idx].storage);
        } else {
            strlcpy(o, a, n);
        }
        return;
    }
    fmt_energy(v, a, sizeof(a));
    double neg = idx >= 0 ? f->nodes[idx].vneg : 0;
    if (fabs(neg) >= 0.0005) {
        fmt_energy(fabs(neg), b, sizeof(b));
        snprintf(o, n, LV_SYMBOL_DOWN "%s  " LV_SYMBOL_UP "%s", a, b);
    } else {
        strlcpy(o, a, n);
    }
}

static void detail_update(ui_efm_t *f)
{
    if (f->rebuild) {
        f->rebuild = false;
        build_diagram(f);
        restart_queries(f);
    }
    poll(f);
    take_results(f);
    if (f->show_chart) {
        chart_poll(f);
        chart_legend_update(f);
    }

    char buf[64];
    const bool live = f->view == V_ACTUAL;

    /* Zentrum */
    double hv = 0;
    bool hv_ok;
    if (f->focus >= 0) {
        hv_ok = node_value(f, f->focus, &hv);
    } else if (live) {
        hv_ok = f->cpwr_ok ? (hv = f->cpwr, true) : live_consumption(f, &hv);
    } else {
        hv_ok = f->sum_ok[S_CONS];
        hv = f->sum[S_CONS];
    }
    if (hv_ok) {
        if (live) fmt_power(hv, buf, sizeof(buf));
        else fmt_energy(hv, buf, sizeof(buf));
        lv_label_set_text(f->hub_val, buf);
    } else {
        lv_label_set_text(f->hub_val, "-");
    }

    /* Knoten */
    double sum_children = 0;
    bool all_ok = true;
    for (int i = 0; i < f->nvis; i++) {
        vis_t *vs = &f->vis[i];
        if (vs->node < 0) continue;
        double v;
        bool ok = node_value(f, vs->node, &v);
        if (ok) {
            sum_children += v;
            value_text(f, vs->node, v, buf, sizeof(buf));
            lv_label_set_text(vs->val, buf);
        } else {
            all_ok = false;
            lv_label_set_text(vs->val, "-");
        }
        int dir = 0;
        if (live && ok && fabs(v) >= 0.005) {
            int sign = v > 0 ? 1 : -1;
            /* Quellen fliessen zum Zentrum; Unterzähler (gleicher Typ wie der Fokus) sind Verbraucher */
            int t = f->nodes[vs->node].type;
            bool src = is_source(t) && !(f->focus >= 0 && f->nodes[f->focus].type == t);
            dir = src ? sign : -sign;
        }
        set_flow(vs, dir);
        int soc_step = f->nodes[vs->node].storage_ok ? (int)(f->nodes[vs->node].storage / 25) + 1 : 0;
        if (vs->icon_kind == IC_BATT && soc_step && soc_step != vs->soc_drawn) {
            vs->soc_drawn = soc_step;
            draw_icon(vs->icon_box, IC_BATT, lv_obj_get_width(vs->icon_box),
                      lv_color_hex(type_color(NT_STORAGE)), f->nodes[vs->node].storage);
        }
    }
    /* Rest = Fokusknoten - Summe der Unterknoten */
    for (int i = 0; i < f->nvis; i++) {
        vis_t *vs = &f->vis[i];
        if (vs->node != -2) continue;
        if (hv_ok && all_ok) {
            double rest = hv - sum_children;
            if (live) fmt_power(rest, buf, sizeof(buf));
            else fmt_energy(rest, buf, sizeof(buf));
            lv_label_set_text(vs->val, buf);
            set_flow(vs, live && fabs(rest) >= 0.005 ? (rest > 0 ? -1 : 1) : 0);
        } else {
            lv_label_set_text(vs->val, "-");
            set_flow(vs, 0);
        }
    }

    /* Kennzahlen */
    for (int i = 0; i < S_COUNT; i++) {
        if (!f->sum_ok[i]) {
            lv_label_set_text(f->card_val[i], "-");
            continue;
        }
        double v = f->sum[i];
        if (i == S_INCOME) {
            snprintf(buf, sizeof(buf), "%.2f %s", v, f->currency);
            comma(buf);
        } else if (i == S_CO2) {
            snprintf(buf, sizeof(buf), v < 99.95 ? "%.1f kg" : "%.0f kg", v);
            comma(buf);
        } else {
            fmt_energy(v, buf, sizeof(buf));
        }
        lv_label_set_text(f->card_val[i], buf);
    }
}

void ui_efm_update(ui_efm_t *f)
{
    if (f == NULL) return;
    if (f->detail) detail_update(f);
    else tile_update(f);
}

void ui_efm_free(ui_efm_t *f)
{
    if (f == NULL) return;
    stop_anims(f);
    heap_caps_free(f);
}

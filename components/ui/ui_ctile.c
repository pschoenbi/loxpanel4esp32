/*
 * Kompakte Kacheln und Popups.
 */

#include "ui_ctile.h"
#include "ui_door.h"
#include "ui_efm.h"
#include "ui_irc.h"
#include "ui_priv.h"
#include "ui_widgets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lox_client.h"
#include "lox_struct.h"

static const char *TAG = "ui_tile";

typedef enum { K_SWITCH, K_PUSH, K_DIMMER, K_JAL, K_LIGHT, K_IRC, K_EFM, K_CAM, K_GATE, K_NFC, K_WINMON, K_INFO_A, K_INFO_D, K_TEXT, K_METER, K_GENERIC,
               K_UNASSIGNED } kind_t;

typedef struct {
    lox_uuid_t u;
    bool ok;
} st_t;

struct ui_ctile {
    kind_t kind;
    char uuid[48];
    char name[48];
    bool secured;
    st_t s_main, s_min, s_max, s_up, s_down, s_total;
    char fmt[24], fmt2[24];             // Zahlenformat (Info/Zähler)
    char txt_on[24], txt_off[24];       // InfoOnlyDigital
    uint32_t col_on, col_off;           // InfoOnlyDigital-Farben aus Loxone (0 = Standard)
    lv_obj_t *obj, *icon, *state, *slider;
    lv_obj_t *jb[2];
    lv_obj_t *sub;                      // (alt) Raumregler-Zeile
    ui_irc_t *irc;                      // Raumregler-Kachel (eigenes Modul)
    ui_efm_t *efm;                      // Energieflussmonitor-Kachel
    bool held[2];
    bool rang;                          // Kamera: Klingel war schon an
    bool dragging;
    cfg_tile_t cfg;                     // Kopie (für Auswahl)
};

/* ---------- Hilfen ---------- */

static void st(const cJSON *ctrl, const char *name, st_t *out)
{
    const cJSON *s = cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "states"), name);
    out->ok = cJSON_IsString(s) && lox_uuid_parse(s->valuestring, &out->u);
}

static bool val(const st_t *s, double *v)
{
    return s->ok && lox_value_get(&s->u, v);
}

static void send(ui_ctile_t *t, const char *cmd)
{
    if (t->secured) {
        ESP_LOGW(TAG, "%s: gesicherter Baustein - Befehl nicht gesendet", t->name);
        return;
    }
    ESP_LOGI(TAG, "%s: %s", t->name, cmd);
    if (lox_send_command(t->uuid, cmd) != ESP_OK) {
        ESP_LOGW(TAG, "%s: Miniserver nicht online - Befehl verworfen", t->name);
    }
}

/* Loxone-Format ("%.1f°C", "%.0f W", "%d %%") sicher anwenden: nur eine Zahl-Umwandlung erlaubt */
static void fmt_value(const char *fmt, double v, char *out, size_t size)
{
    int conv = 0;
    char kind = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') { p++; continue; }
        const char *q = p + 1;
        while (*q && strchr("0123456789.-+ ", *q)) q++;
        if (*q == 'f' || *q == 'g' || *q == 'd' || *q == 'i') { conv++; kind = *q; p = q; }
        else { conv = 99; break; }
    }
    if (conv == 1) {
        if (kind == 'd' || kind == 'i') snprintf(out, size, fmt, (int)lround(v));
        else snprintf(out, size, fmt, v);
    } else {
        snprintf(out, size, "%.1f", v);
    }
}

static void copy_fmt(const cJSON *ctrl, const char *key, char *out, size_t size, const char *def)
{
    const cJSON *f = cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "details"), key);
    strlcpy(out, cJSON_IsString(f) ? f->valuestring : def, size);
}

static lv_obj_t *small_btn(lv_obj_t *parent, const char *sym, int size)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_center(l);
    return b;
}

/* ---------- Ereignisse ---------- */

static void tile_clicked(lv_event_t *e)
{
    ui_ctile_t *t = lv_event_get_user_data(e);
    double v = 0;
    switch (t->kind) {
    case K_SWITCH:
        val(&t->s_main, &v);
        send(t, v > 0.5 ? "off" : "on");
        break;
    case K_PUSH:
        send(t, "pulse");
        break;
    case K_DIMMER: {
        double mn = 0;
        val(&t->s_min, &mn);
        val(&t->s_main, &v);
        send(t, v > mn ? "off" : "on");
        break;
    }
    case K_LIGHT: {
        int ids[8];
        int n = t->s_main.ok ? ui_light_active_moods(&t->s_main.u, ids, 8) : -1;
        send(t, ui_light_is_off(ids, n) ? "plus" : "changeTo/778");
        break;
    }
    case K_UNASSIGNED:
        ui_popup_picker(&t->cfg);
        break;
    case K_EFM:
    case K_CAM:
    case K_GATE:
    case K_NFC:
    case K_WINMON:
        ui_popup_control(t->uuid, &t->cfg, NULL);
        break;
    default:
        break;
    }
}

static void tile_long(lv_event_t *e)
{
    ui_ctile_t *t = lv_event_get_user_data(e);
    if (t->kind == K_UNASSIGNED) {
        ui_popup_picker(&t->cfg);
    } else {
        /* Raumregler-Kacheln haben kein Zustandsfeld (eigene Darstellung) */
        ui_popup_control(t->uuid, &t->cfg, t->state ? lv_label_get_text(t->state) : NULL);
    }
}

static void slider_event(lv_event_t *e)
{
    ui_ctile_t *t = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        t->dragging = true;
    } else if (c == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text_fmt(t->state, "%d %%", (int)lv_slider_get_value(t->slider));
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        t->dragging = false;
        double mn = 0, mx = 100;
        val(&t->s_min, &mn);
        val(&t->s_max, &mx);
        if (mx <= mn) mx = mn + 100;
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "%d", (int)lround(mn + (mx - mn) * lv_slider_get_value(t->slider) / 100.0));
        send(t, cmd);
    }
}

/* Raumregler: Komfort-Temperatur vorübergehend um 0,5 °C ändern (wie in der Loxone-App).
   Laut Doku: comfortTemperature + comfortTemperatureOffset ± 0.5 -> setComfortModeTemp */
static void irc_btn(lv_event_t *e)
{
    ui_ctile_t *t = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    double comfort = 0, offset = 0;
    if (!val(&t->s_min, &comfort)) return;
    val(&t->s_max, &offset);
    double nt = comfort + offset + (b == t->jb[1] ? 0.5 : -0.5);
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "setComfortModeTemp/%.1f", nt);
    send(t, cmd);
}

const char *ui_irc_mode_text(int mode)
{
    switch (mode) {
    case 0: return "Eco";
    case 1: return "Komfort";
    case 2: return "Gebäudeschutz";
    case 3: return "Manuell";
    case 4: return "Aus";
    default: return "";
    }
}

static void jal_btn(lv_event_t *e)
{
    ui_ctile_t *t = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    lv_event_code_t c = lv_event_get_code(e);
    int i = b == t->jb[0] ? 0 : 1;
    if (c == LV_EVENT_LONG_PRESSED) {
        t->held[i] = true;
        send(t, i == 0 ? "up" : "down");
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (t->held[i]) {
            t->held[i] = false;
            send(t, i == 0 ? "UpOff" : "DownOff");
        } else if (c == LV_EVENT_RELEASED) {
            send(t, i == 0 ? "FullUp" : "FullDown");
        }
    }
}

/* ---------- Erzeugen ---------- */

static kind_t kind_of(const char *type)
{
    if (!type) return K_GENERIC;
    if (!strcmp(type, "Switch")) return K_SWITCH;
    if (!strcmp(type, "Pushbutton")) return K_PUSH;
    if (!strcmp(type, "Dimmer") || !strcmp(type, "EIBDimmer")) return K_DIMMER;
    if (!strcmp(type, "Jalousie")) return K_JAL;
    if (!strcmp(type, "LightControllerV2")) return K_LIGHT;
    if (!strcmp(type, "IRoomControllerV2")) return K_IRC;
    if (!strcmp(type, "EFM")) return K_EFM;
    if (!strcmp(type, "Intercom") || !strcmp(type, "IntercomV2")) return K_CAM;
    if (!strcmp(type, "Gate")) return K_GATE;
    if (!strcmp(type, "NfcCodeTouch")) return K_NFC;
    if (!strcmp(type, "WindowMonitor")) return K_WINMON;
    if (!strcmp(type, "InfoOnlyAnalog")) return K_INFO_A;
    if (!strcmp(type, "InfoOnlyDigital")) return K_INFO_D;
    if (!strcmp(type, "TextState")) return K_TEXT;
    if (!strcmp(type, "Meter")) return K_METER;
    return K_GENERIC;
}

/* Symbol aus Bausteintyp und Loxone-Kategorie (Kategorienamen sind frei wählbar,
   daher nach Stichworten) */
static const char *icon_of(kind_t k, const char *cat)
{
    if (k == K_UNASSIGNED) return LV_SYMBOL_WARNING;
    if (k == K_SWITCH || k == K_PUSH || k == K_LIGHT || k == K_DIMMER) return LV_SYMBOL_POWER;
    if (k == K_JAL) return LV_SYMBOL_BARS;
    if (k == K_IRC) return LV_SYMBOL_HOME;
    if (k == K_CAM) return LV_SYMBOL_VIDEO;
    if (k == K_GATE) return LV_SYMBOL_HOME;
    if (k == K_NFC) return LV_SYMBOL_KEYBOARD;
    if (k == K_WINMON) return LV_SYMBOL_EYE_OPEN;
    if (cat) {
        if (strcasestr(cat, "alarm") || strcasestr(cat, "sicher") || strcasestr(cat, "rauch")) return LV_SYMBOL_BELL;
        if (strcasestr(cat, "feucht") || strcasestr(cat, "klima") || strcasestr(cat, "wasser") ||
            strcasestr(cat, "bewässer")) return LV_SYMBOL_TINT;
        if (strcasestr(cat, "energ") || strcasestr(cat, "photov") || strcasestr(cat, "strom") ||
            strcasestr(cat, "auto")) return LV_SYMBOL_CHARGE;
        if (strcasestr(cat, "audio") || strcasestr(cat, "musik") || strcasestr(cat, "media")) return LV_SYMBOL_AUDIO;
        if (strcasestr(cat, "wetter")) return LV_SYMBOL_EYE_OPEN;
    }
    if (k == K_METER) return LV_SYMBOL_CHARGE;
    if (k == K_INFO_A || k == K_INFO_D || k == K_TEXT) return LV_SYMBOL_EYE_OPEN;
    return LV_SYMBOL_SETTINGS;
}

static const char *category_of(const cJSON *ctrl)
{
    const cJSON *cat = cJSON_GetObjectItem(ctrl, "cat");
    if (!cJSON_IsString(cat)) return NULL;
    const cJSON *c = cJSON_GetObjectItem(cJSON_GetObjectItem(lox_struct_root(), "cats"), cat->valuestring);
    const cJSON *n = cJSON_GetObjectItem(c, "name");
    return cJSON_IsString(n) ? n->valuestring : NULL;
}

static uint32_t parse_color(const cJSON *v)
{
    if (!cJSON_IsString(v) || v->valuestring[0] != '#') return 0;
    return (uint32_t)strtoul(v->valuestring + 1, NULL, 16) | 0x01000000;   // Bit 24 = gesetzt
}

ui_ctile_t *ui_ctile_create(lv_obj_t *parent, const cfg_tile_t *ct, const cJSON *ctrl, const char *room,
                            int w, int h)
{
    ui_ctile_t *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_SPIRAM);
    if (t == NULL) return NULL;
    t->cfg = *ct;

    const cJSON *type = ctrl ? cJSON_GetObjectItem(ctrl, "type") : NULL;
    const cJSON *name = ctrl ? cJSON_GetObjectItem(ctrl, "name") : NULL;
    const cJSON *uuid = ctrl ? cJSON_GetObjectItem(ctrl, "uuidAction") : NULL;
    t->kind = ctrl ? kind_of(cJSON_IsString(type) ? type->valuestring : NULL) : K_UNASSIGNED;
    if (cJSON_IsString(uuid)) strlcpy(t->uuid, uuid->valuestring, sizeof(t->uuid));
    strlcpy(t->name, ct->titel[0] ? ct->titel : cJSON_IsString(name) ? name->valuestring : ct->ziel, sizeof(t->name));
    t->secured = ctrl && cJSON_IsTrue(cJSON_GetObjectItem(ctrl, "isSecured"));

    switch (t->kind) {
    case K_SWITCH: case K_PUSH: st(ctrl, "active", &t->s_main); break;
    case K_DIMMER:
        st(ctrl, "position", &t->s_main); st(ctrl, "min", &t->s_min); st(ctrl, "max", &t->s_max); break;
    case K_JAL:
        st(ctrl, "position", &t->s_main); st(ctrl, "up", &t->s_up); st(ctrl, "down", &t->s_down); break;
    case K_LIGHT:
        st(ctrl, "activeMoods", &t->s_main); st(ctrl, "moodList", &t->s_total); break;
    case K_IRC:
        st(ctrl, "tempActual", &t->s_main); st(ctrl, "tempTarget", &t->s_total);
        st(ctrl, "comfortTemperature", &t->s_min); st(ctrl, "comfortTemperatureOffset", &t->s_max);
        st(ctrl, "activeMode", &t->s_up);
        copy_fmt(ctrl, "format", t->fmt, sizeof(t->fmt), "%.1f°");
        break;
    case K_INFO_A:
        st(ctrl, "value", &t->s_main); copy_fmt(ctrl, "format", t->fmt, sizeof(t->fmt), "%.1f"); break;
    case K_INFO_D: {
        st(ctrl, "active", &t->s_main);
        const cJSON *tx = cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "details"), "text");
        const cJSON *on = cJSON_GetObjectItem(tx, "on"), *off = cJSON_GetObjectItem(tx, "off");
        strlcpy(t->txt_on, cJSON_IsString(on) ? on->valuestring : "Ein", sizeof(t->txt_on));
        strlcpy(t->txt_off, cJSON_IsString(off) ? off->valuestring : "Aus", sizeof(t->txt_off));
        const cJSON *col = cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "details"), "color");
        t->col_on = parse_color(cJSON_GetObjectItem(col, "on"));
        t->col_off = parse_color(cJSON_GetObjectItem(col, "off"));
        break;
    }
    case K_TEXT: st(ctrl, "textAndIcon", &t->s_main); break;
    case K_CAM: st(ctrl, "bell", &t->s_main); break;
    case K_GATE: st(ctrl, "position", &t->s_main); st(ctrl, "active", &t->s_up); break;
    case K_NFC: st(ctrl, "historyDate", &t->s_main); break;
    case K_WINMON:
        st(ctrl, "numOpen", &t->s_main); st(ctrl, "numTilted", &t->s_up);
        st(ctrl, "numUnlocked", &t->s_down); st(ctrl, "numOffline", &t->s_total);
        break;
    case K_METER:
        st(ctrl, "actual", &t->s_main); st(ctrl, "total", &t->s_total);
        copy_fmt(ctrl, "actualFormat", t->fmt, sizeof(t->fmt), "%.2f kW");
        copy_fmt(ctrl, "totalFormat", t->fmt2, sizeof(t->fmt2), "%.1f kWh");
        break;
    default: break;
    }

    lv_obj_t *o = ui_tile_create(parent);
    t->obj = o;
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(o, ui_px(12), 0);
    lv_obj_set_style_pad_row(o, ui_px(4), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(UI_COL_ON), 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, tile_clicked, LV_EVENT_SHORT_CLICKED, t);
    lv_obj_add_event_cb(o, tile_long, LV_EVENT_LONG_PRESSED, t);

    /* Raumregler: eigene Darstellung wie in der Loxone-App */
    if (t->kind == K_IRC) {
        t->irc = ui_irc_tile_create(o, ctrl, ct->titel, w, h);
        return t;
    }
    if (t->kind == K_EFM) {
        t->efm = ui_efm_tile_create(o, ctrl, ct->titel, room, w, h);
        return t;
    }

    t->icon = lv_label_create(o);
    lv_obj_set_style_text_font(t->icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t->icon, lv_color_hex(t->kind == K_UNASSIGNED ? UI_COL_WARN : UI_COL_DIM), 0);
    lv_label_set_text(t->icon, icon_of(t->kind, ctrl ? category_of(ctrl) : NULL));

    lv_obj_t *n = lv_label_create(o);
    lv_obj_set_width(n, LV_PCT(100));
    lv_obj_set_style_text_font(n, ui_font(UI_F_S), 0);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_height(n, ui_px(46), 0);
    if (t->kind == K_UNASSIGNED) {
        lv_label_set_text_fmt(n, "%s", ct->titel[0] ? ct->titel : ct->ziel);
    } else if (ct->titel[0]) {
        lv_label_set_text(n, ct->titel);
    } else {
        lv_label_set_text(n, cJSON_IsString(name) ? name->valuestring : "?");
    }

    lv_obj_t *r = lv_label_create(o);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_style_text_font(r, ui_font(UI_F_S), 0);
    lv_obj_set_style_text_color(r, lv_color_hex(UI_COL_DIM), 0);
    lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
    if (t->kind == K_UNASSIGNED) {
        lv_label_set_text(r, ct->bind == CFG_BIND_AMBIGUOUS ? "mehrdeutig - antippen"
                           : ct->bind == CFG_BIND_PENDING ? "warte auf Miniserver"
                           : "nicht zugeordnet - antippen");
    } else if (t->kind == K_GENERIC) {
        lv_label_set_text_fmt(r, "%s (%s)", room ? room : "", cJSON_IsString(type) ? type->valuestring : "?");
    } else {
        lv_label_set_text(r, room ? room : "");
    }

    /* Platzhalter, damit Zustand/Bedienung unten sitzen */
    lv_obj_t *sp = lv_obj_create(o);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    const bool big = h >= 2;
    const bool wide = w >= 2;

    if (t->kind == K_IRC) {
        t->sub = lv_label_create(o);
        lv_obj_set_width(t->sub, LV_PCT(100));
        lv_obj_set_style_text_font(t->sub, ui_font(UI_F_S), 0);
        lv_obj_set_style_text_color(t->sub, lv_color_hex(UI_COL_DIM), 0);
        lv_label_set_long_mode(t->sub, LV_LABEL_LONG_DOT);
        lv_label_set_text(t->sub, "");
    }

    if (t->kind == K_JAL || t->kind == K_IRC) {
        lv_obj_t *row = lv_obj_create(o);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        t->state = lv_label_create(row);
        lv_obj_set_style_text_font(t->state, ui_font(UI_F_M), 0);
        lv_obj_t *btns = lv_obj_create(row);
        lv_obj_remove_style_all(btns);
        lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(btns, ui_px(8), 0);
        int bs = ui_px(big || wide ? 60 : 46);
        if (t->kind == K_IRC) {
            lv_obj_set_style_text_font(t->state, ui_font(big ? UI_F_XL : UI_F_L), 0);
            t->jb[0] = small_btn(btns, LV_SYMBOL_MINUS, bs);
            t->jb[1] = small_btn(btns, LV_SYMBOL_PLUS, bs);
            for (int i = 0; i < 2; i++) lv_obj_add_event_cb(t->jb[i], irc_btn, LV_EVENT_CLICKED, t);
        } else {
            t->jb[0] = small_btn(btns, LV_SYMBOL_UP, bs);
            t->jb[1] = small_btn(btns, LV_SYMBOL_DOWN, bs);
            for (int i = 0; i < 2; i++) lv_obj_add_event_cb(t->jb[i], jal_btn, LV_EVENT_ALL, t);
        }
    } else {
        t->state = lv_label_create(o);
        lv_obj_set_width(t->state, LV_PCT(100));
        lv_obj_set_style_text_font(t->state, ui_font(big ? UI_F_L : UI_F_M), 0);
        lv_label_set_long_mode(t->state, LV_LABEL_LONG_DOT);
    }
    lv_label_set_text(t->state, t->kind == K_UNASSIGNED || t->kind == K_GENERIC ? "" : "-");

    if (t->kind == K_DIMMER && (wide || big)) {
        t->slider = lv_slider_create(o);
        lv_obj_set_size(t->slider, LV_PCT(92), ui_px(26));
        lv_obj_set_style_margin_bottom(t->slider, ui_px(6), 0);
        lv_obj_align(t->slider, LV_ALIGN_CENTER, 0, 0);
        lv_slider_set_range(t->slider, 0, 100);
        lv_obj_add_event_cb(t->slider, slider_event, LV_EVENT_ALL, t);
    }

    ui_ctile_update(t);
    return t;
}

void ui_ctile_update(ui_ctile_t *t)
{
    if (t == NULL) return;
    if (t->irc) {
        ui_irc_update(t->irc);
        return;
    }
    if (t->efm) {
        ui_efm_update(t->efm);
        return;
    }
    double v = 0;
    bool active = false;
    char buf[64];

    switch (t->kind) {
    case K_SWITCH:
    case K_PUSH:
        if (val(&t->s_main, &v)) {
            active = v > 0.5;
            if (t->kind == K_SWITCH) lv_label_set_text(t->state, active ? "Ein" : "Aus");
            else lv_label_set_text(t->state, "Taster");
        }
        break;
    case K_DIMMER: {
        double mn = 0, mx = 100;
        val(&t->s_min, &mn);
        val(&t->s_max, &mx);
        if (mx <= mn) mx = mn + 100;
        if (val(&t->s_main, &v)) {
            int pct = (int)lround((v - mn) * 100.0 / (mx - mn));
            pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
            active = pct > 0;
            if (!t->dragging) {
                if (pct == 0) lv_label_set_text(t->state, "Aus");
                else lv_label_set_text_fmt(t->state, "%d %%", pct);
                if (t->slider) lv_slider_set_value(t->slider, pct, LV_ANIM_OFF);
            }
        }
        break;
    }
    case K_JAL: {
        double up = 0, down = 0;
        val(&t->s_up, &up);
        val(&t->s_down, &down);
        active = up > 0.5 || down > 0.5;
        if (val(&t->s_main, &v)) {
            int pct = (int)lround(v * 100);
            if (active) lv_label_set_text(t->state, up > 0.5 ? LV_SYMBOL_UP " ..." : LV_SYMBOL_DOWN " ...");
            else if (pct <= 0) lv_label_set_text(t->state, "Offen");
            else if (pct >= 100) lv_label_set_text(t->state, "Zu");
            else lv_label_set_text_fmt(t->state, "%d %%", pct);
        }
        break;
    }
    case K_IRC: {
        double target = 0, mode = -1;
        if (val(&t->s_main, &v)) {
            fmt_value(t->fmt, v, buf, sizeof(buf));
            lv_label_set_text(t->state, buf);
        }
        bool has_t = val(&t->s_total, &target);
        val(&t->s_up, &mode);
        char tbuf[24] = "";
        if (has_t) fmt_value(t->fmt, target, tbuf, sizeof(tbuf));
        lv_label_set_text_fmt(t->sub, "Soll %s  %s", has_t ? tbuf : "-", ui_irc_mode_text((int)mode));
        break;
    }
    case K_LIGHT: {
        int ids[8];
        int n = t->s_main.ok ? ui_light_active_moods(&t->s_main.u, ids, 8) : -1;
        if (n >= 0) {
            active = !ui_light_is_off(ids, n);
            ui_light_mood_text(&t->s_total.u, ids, n, buf, sizeof(buf));
            lv_label_set_text(t->state, buf);
        }
        break;
    }
    case K_INFO_A:
        if (val(&t->s_main, &v)) {
            fmt_value(t->fmt, v, buf, sizeof(buf));
            lv_label_set_text(t->state, buf);
        }
        break;
    case K_INFO_D:
        if (val(&t->s_main, &v)) {
            bool on = v > 0.5;
            lv_label_set_text(t->state, on ? t->txt_on : t->txt_off);
            uint32_t c = on ? t->col_on : t->col_off;
            lv_obj_set_style_text_color(t->state, lv_color_hex(c ? (c & 0xffffff) : UI_COL_TEXT), 0);
        }
        break;
    case K_TEXT:
        if (t->s_main.ok && lox_text_get(&t->s_main.u, buf, sizeof(buf))) {
            lv_label_set_text(t->state, buf);
        }
        break;
    case K_METER:
        if (val(&t->s_main, &v)) {
            fmt_value(t->fmt, v, buf, sizeof(buf));
            lv_label_set_text(t->state, buf);
        }
        break;
    case K_CAM: {
        bool ring = val(&t->s_main, &v) && v > 0.5;
        active = ring;
        lv_label_set_text(t->state, ring ? "Es klingelt!" : "Live-Bild");
        lv_obj_set_style_text_color(t->state, lv_color_hex(ring ? UI_COL_WARN : UI_COL_TEXT), 0);
        /* Wie in der App: beim Klingeln das Livebild öffnen */
        if (ring && !t->rang) ui_popup_control(t->uuid, &t->cfg, NULL);
        t->rang = ring;
        break;
    }
    case K_GATE:
        if (val(&t->s_main, &v)) {
            double act = 0;
            val(&t->s_up, &act);
            ui_gate_text(v, act, buf, sizeof(buf));
            lv_label_set_text(t->state, buf);
            active = v > 0.01 || fabs(act) > 0.5;
        }
        break;
    case K_NFC:
        if (val(&t->s_main, &v) && v > 0) {
            time_t ts = (time_t)(v > 1e11 ? v / 1000 : v);
            struct tm tm;
            localtime_r(&ts, &tm);
            snprintf(buf, sizeof(buf), "Zutritt %02d.%02d. %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
            lv_label_set_text(t->state, buf);
        } else {
            lv_label_set_text(t->state, "Zutritt");
        }
        break;
    case K_WINMON: {
        double o = 0, ti = 0, un = 0, off = 0;
        bool ok = val(&t->s_main, &o);
        val(&t->s_up, &ti);
        val(&t->s_down, &un);
        val(&t->s_total, &off);
        if (ok) {
            active = ui_winmon_text(o, ti, un, off, buf, sizeof(buf));
            lv_label_set_text(t->state, buf);
            lv_obj_set_style_text_color(t->state, lv_color_hex(active ? UI_COL_WARN : UI_COL_TEXT), 0);
        }
        break;
    }
    default:
        break;
    }

    lv_obj_set_style_border_width(t->obj, active ? ui_px(2) : 0, 0);
    if (t->kind != K_UNASSIGNED) {
        lv_obj_set_style_text_color(t->icon, lv_color_hex(active ? UI_COL_ON : UI_COL_DIM), 0);
    }
}

void ui_ctile_free(ui_ctile_t *t)
{
    if (t) {
        ui_irc_free(t->irc);
        ui_efm_free(t->efm);
    }
    heap_caps_free(t);
}

/* ------------------------------------------------------------------ */
/* Popups                                                              */

static lv_obj_t *s_pop;             // Vollbild-Hintergrund auf lv_layer_top
static ui_widget_t *s_pop_widget;
static cfg_tile_t s_pick_tile;
typedef struct {
    char uuid[48];
    char label[120];        // "Raum: Name"
    char room[48];
    char name[64];
} pick_t;
static pick_t *s_picks;
static int s_pick_count;

void ui_popup_close(void)
{
    if (s_pop_widget) {
        ui_widget_destroy(s_pop_widget);
        s_pop_widget = NULL;
    }
    if (s_pop) {
        lv_obj_delete_async(s_pop);     // kann aus einem Ereignis eines Kindobjekts kommen
        s_pop = NULL;
    }
    heap_caps_free(s_picks);
    s_picks = NULL;
    s_pick_count = 0;
}

static void bg_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        ui_popup_close();
    }
}

static void close_clicked(lv_event_t *e)
{
    ui_popup_close();
}

/* Halbtransparenter Hintergrund + Fenster mit Schliessen-Knopf */
static lv_obj_t *popup_frame(const char *title)
{
    ui_popup_close();
    s_pop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_pop);
    lv_obj_set_size(s_pop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_pop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pop, LV_OPA_60, 0);
    lv_obj_add_flag(s_pop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pop, bg_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *win = ui_tile_create(s_pop);
    lv_obj_set_size(win, LV_PCT(62), LV_PCT(82));
    lv_obj_center(win);
    lv_obj_set_style_pad_all(win, ui_px(18), 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(0x2a323d), 0);

    lv_obj_t *hdr = lv_label_create(win);
    lv_obj_set_style_text_font(hdr, ui_font(UI_F_S), 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_set_width(hdr, LV_PCT(85));
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_DOT);
    lv_label_set_text(hdr, title);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *x = lv_button_create(win);
    lv_obj_set_size(x, ui_px(48), ui_px(48));
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, ui_px(6), -ui_px(6));
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, &lv_font_montserrat_28, 0);
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, close_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *body = lv_obj_create(win);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), lv_pct(88));
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    return body;
}

static void hide_clicked(lv_event_t *e);
static void change_clicked(lv_event_t *e);

static lv_obj_t *footer_button(lv_obj_t *row, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, ui_font(UI_F_S), 0);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* Lang drücken: Bedienung bzw. Anzeige des Bausteins + Ausblenden / Baustein ändern */
void ui_popup_control(const char *uuid_action, const cfg_tile_t *ct, const char *value_text)
{
    if (!lox_struct_lock(200)) return;
    const char *room = NULL;
    const cJSON *ctrl = lox_struct_find_control(uuid_action, &room);
    lv_obj_t *body = popup_frame("");
    lv_obj_set_height(body, lv_pct(74));
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, ui_px(34));
    if (ct) s_pick_tile = *ct;

    const cJSON *type = ctrl ? cJSON_GetObjectItem(ctrl, "type") : NULL;
    /* Raumregler: grosses Fenster wie in der App */
    if (cJSON_IsString(type) && (!strcmp(type->valuestring, "IRoomControllerV2") ||
                                 !strcmp(type->valuestring, "EFM") || !strcmp(type->valuestring, "Intercom") ||
                                 !strcmp(type->valuestring, "IntercomV2") ||
                                 !strcmp(type->valuestring, "WindowMonitor"))) {
        lv_obj_t *win = lv_obj_get_parent(body);
        lv_obj_set_size(win, LV_PCT(96), LV_PCT(94));
        lv_obj_set_style_bg_color(win, lv_color_hex(0x16191d), 0);
        lv_obj_set_height(body, lv_pct(84));
        lv_obj_align(body, LV_ALIGN_TOP_MID, 0, ui_px(10));
    }
    if (ctrl && ui_widget_supports(cJSON_IsString(type) ? type->valuestring : NULL)) {
        s_pop_widget = ui_widget_create(body, ctrl, room);
    } else {
        /* Nur Anzeige: Name, Raum/Kategorie, Wert */
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(body, ui_px(12), 0);
        const cJSON *name = ctrl ? cJSON_GetObjectItem(ctrl, "name") : NULL;
        lv_obj_t *n = lv_label_create(body);
        lv_obj_set_style_text_font(n, ui_font(UI_F_L), 0);
        lv_label_set_text(n, cJSON_IsString(name) ? name->valuestring : (ct ? ct->ziel : "?"));
        lv_obj_t *r = lv_label_create(body);
        lv_obj_set_style_text_font(r, ui_font(UI_F_S), 0);
        lv_obj_set_style_text_color(r, lv_color_hex(UI_COL_DIM), 0);
        const char *cat = ctrl ? category_of(ctrl) : NULL;
        lv_label_set_text_fmt(r, "%s%s%s  (%s)", room ? room : "", cat ? " / " : "", cat ? cat : "",
                              cJSON_IsString(type) ? type->valuestring : "-");
        lv_obj_t *v = lv_label_create(body);
        lv_obj_set_style_text_font(v, ui_font(UI_F_XL), 0);
        lv_label_set_text(v, value_text ? value_text : "");
    }
    lox_struct_unlock();

    if (ct) {
        lv_obj_t *row = lv_obj_create(lv_obj_get_parent(body));
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, ui_px(12), 0);
        lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
        if (!ct->from_auto) footer_button(row, "Baustein ändern", change_clicked);
        footer_button(row, "Ausblenden", hide_clicked);
    }
}

static int cmp_pick(const void *a, const void *b)
{
    return strcasecmp(((const pick_t *)a)->label, ((const pick_t *)b)->label);
}

static void collect_picks(const cJSON *ctrls, const char *parent_room, int *cap)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, ctrls) {
        const char *room = lox_struct_room_of(c);
        if (room == NULL) room = parent_room;
        const cJSON *type = cJSON_GetObjectItem(c, "type");
        const cJSON *name = cJSON_GetObjectItem(c, "name");
        const cJSON *uuid = cJSON_GetObjectItem(c, "uuidAction");
        if (cJSON_IsString(type) && cJSON_IsString(uuid) && kind_of(type->valuestring) != K_GENERIC) {
            if (s_pick_count == *cap) {
                *cap = *cap ? *cap * 2 : 64;
                s_picks = heap_caps_realloc(s_picks, *cap * sizeof(pick_t), MALLOC_CAP_SPIRAM);
                if (s_picks == NULL) return;
            }
            pick_t *p = &s_picks[s_pick_count++];
            strlcpy(p->uuid, uuid->valuestring, sizeof(p->uuid));
            strlcpy(p->room, room ? room : "", sizeof(p->room));
            strlcpy(p->name, cJSON_IsString(name) ? name->valuestring : "?", sizeof(p->name));
            /* Unterscheidbar machen: Typ, Favorit, beim Raumregler die Ist-Temperatur */
            static const char *const tnames[][2] = {
                { "Switch", "Schalter" }, { "Pushbutton", "Taster" }, { "Dimmer", "Dimmer" },
                { "EIBDimmer", "Dimmer" }, { "Jalousie", "Jalousie" }, { "LightControllerV2", "Licht" },
                { "IRoomControllerV2", "Raumregler" }, { "Meter", "Zähler" }, { "InfoOnlyAnalog", "Wert" },
                { "InfoOnlyDigital", "Info" }, { "TextState", "Text" }, { "EFM", "Energiefluss" }, { "Intercom", "Kamera" }, { "IntercomV2", "Kamera" }, { "Gate", "Tor" }, { "NfcCodeTouch", "Zutritt" },
                { "WindowMonitor", "Tür-/Fensterüberwachung" },
            };
            const char *tn = type->valuestring;
            for (size_t i = 0; i < sizeof(tnames) / sizeof(tnames[0]); i++) {
                if (!strcmp(tn, tnames[i][0])) tn = tnames[i][1];
            }
            char extra[24] = "";
            if (kind_of(type->valuestring) == K_IRC) {
                st_t ta;
                double v;
                st(c, "tempActual", &ta);
                if (val(&ta, &v)) snprintf(extra, sizeof(extra), ", %.1f°", v);
            }
            snprintf(p->label, sizeof(p->label), "%.40s: %.40s  (%s%s)%s", p->room, p->name, tn, extra,
                     cJSON_IsTrue(cJSON_GetObjectItem(c, "isFavorite")) ? "  - Favorit" : "");
        }
        collect_picks(cJSON_GetObjectItem(c, "subControls"), room, cap);
    }
}

static void pick_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_pick_count) return;
    const pick_t *p = &s_picks[idx];
    /* "Raum/Name", wenn eindeutig (klonbar); sonst feste UUID */
    int same = 0;
    for (int i = 0; i < s_pick_count; i++) {
        if (!strcasecmp(s_picks[i].room, p->room) && !strcasecmp(s_picks[i].name, p->name)) same++;
    }
    char ziel[128];
    if (same == 1 && p->room[0]) snprintf(ziel, sizeof(ziel), "%s/%s", p->room, p->name);
    else snprintf(ziel, sizeof(ziel), "uuid:%s", p->uuid);
    char key[72];
    strlcpy(key, s_pick_tile.key, sizeof(key));
    ui_popup_close();
    cfg_set_override(key, ziel, false);
}

static void hide_clicked(lv_event_t *e)
{
    char key[72];
    strlcpy(key, s_pick_tile.key, sizeof(key));
    ui_popup_close();
    cfg_set_override(key, NULL, true);
}

static void change_clicked(lv_event_t *e)
{
    cfg_tile_t ct = s_pick_tile;
    ui_popup_close();
    ui_popup_picker(&ct);
}

void ui_popup_picker(const cfg_tile_t *ct)
{
    if (!lox_struct_loaded() || !lox_struct_lock(200)) return;
    s_pick_tile = *ct;
    char title[160];
    snprintf(title, sizeof(title), "Baustein wählen für \"%s\"", ct->ziel);
    lv_obj_t *body = popup_frame(title);

    int cap = 0;
    collect_picks(cJSON_GetObjectItem(lox_struct_root(), "controls"), NULL, &cap);
    lox_struct_unlock();
    if (s_pick_count > 1) qsort(s_picks, s_pick_count, sizeof(pick_t), cmp_pick);

    /* Name aus dem Ziel: passende Einträge zuerst */
    const char *want = strchr(ct->ziel, '/');
    want = want ? want + 1 : ct->ziel;

    lv_obj_t *list = lv_list_create(body);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(86));
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_text_font(list, ui_font(UI_F_S), 0);

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) lv_list_add_text(list, "Alle Bausteine");
        else lv_list_add_text(list, "Passend");
        for (int i = 0; i < s_pick_count; i++) {
            bool match = want[0] && strcasestr(s_picks[i].name, want) != NULL;
            if ((pass == 0) != match) continue;
            lv_obj_t *b = lv_list_add_button(list, NULL, s_picks[i].label);
            lv_obj_add_event_cb(b, pick_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    lv_obj_t *hide = lv_button_create(body);
    lv_obj_set_style_bg_color(hide, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_align(hide, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_t *hl = lv_label_create(hide);
    lv_obj_set_style_text_font(hl, ui_font(UI_F_S), 0);
    lv_label_set_text(hl, "Kachel ausblenden");
    lv_obj_add_event_cb(hide, hide_clicked, LV_EVENT_CLICKED, NULL);
}

void ui_popup_update(void)
{
    ui_widget_update(s_pop_widget);
}

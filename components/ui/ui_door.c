/*
 * Tür & Zutritt.
 *
 * Laut Loxone "Structure File":
 *   Gate: Status position (1 offen, 0 zu), active (-1 schliesst, 0 steht, 1 öffnet), preventOpen/Close;
 *         Befehle open, close, stop, partiallyOpen.
 *   NfcCodeTouch: details.accessOutputs {q1..q6: Name}; Status historyDate (ms), deviceState;
 *         Befehle output/{1-6}, history -> [{ts, output, type, user, description}].
 *   WindowMonitor: details.windows [{name, room, installPlace}]; Status windowStates (Text "1,4,...",
 *         Bitmaske 1 zu, 2 gekippt, 4 offen, 8 verriegelt, 16 entriegelt, 0 unbekannt),
 *         numOpen, numClosed, numTilted, numOffline, numLocked, numUnlocked.
 */

#include "ui_door.h"
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

static const char *TAG = "ui_door";

enum { D_GATE, D_NFC, D_WIN };

#define MAX_OUT     6
#define MAX_WIN     32
#define MAX_HIST    8

typedef struct {
    lox_uuid_t u;
    bool ok;
} st_t;

struct ui_door {
    int kind;
    char uuid[48], name[64];
    bool secured;
    /* Tor */
    st_t pos, active;
    lv_obj_t *state, *bar;
    /* NFC */
    int nout;
    int out_nr[MAX_OUT];
    st_t hist_date;
    double hist_seen;
    bool hist_seen_ok;
    lv_obj_t *hist_box;
    uint32_t gen;
    /* Fenster */
    st_t win_states;
    int nwin;
    lv_obj_t *win_state[MAX_WIN];
    uint32_t win_hash;
};

bool ui_door_supports(const char *t)
{
    return t && (!strcmp(t, "Gate") || !strcmp(t, "NfcCodeTouch") || !strcmp(t, "WindowMonitor"));
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

static lv_obj_t *label(lv_obj_t *parent, ui_font_id_t f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, ui_font(f), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, ui_px(12), 0);
    return r;
}

static lv_obj_t *btn(lv_obj_t *parent, const char *txt, int w)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, ui_px(64));
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, ui_px(12), 0);
    lv_obj_t *l = label(b, UI_F_M, UI_COL_TEXT, txt);
    lv_obj_set_width(l, w - ui_px(12));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

static void send(ui_door_t *d, const char *cmd)
{
    if (d->secured) {
        ESP_LOGW(TAG, "%s: gesicherter Baustein - Befehl nicht gesendet", d->name);
        return;
    }
    ESP_LOGI(TAG, "%s: %s", d->name, cmd);
    if (lox_send_command(d->uuid, cmd) != ESP_OK) ESP_LOGW(TAG, "Miniserver nicht online");
}

static void cmd_cb(lv_event_t *e)
{
    ui_door_t *d = lv_event_get_user_data(e);
    const char *cmd = lv_obj_get_user_data(lv_event_get_current_target(e));
    if (cmd) send(d, cmd);
}

/* ---------------- Tor ---------------- */

void ui_gate_text(double pos, double active, char *out, size_t size)
{
    if (active > 0.5) snprintf(out, size, "öffnet ...");
    else if (active < -0.5) snprintf(out, size, "schliesst ...");
    else if (pos <= 0.01) snprintf(out, size, "Zu");
    else if (pos >= 0.99) snprintf(out, size, "Offen");
    else snprintf(out, size, "%d %% offen", (int)lround(pos * 100));
}

static void gate_create(ui_door_t *d, lv_obj_t *body, const cJSON *ctrl)
{
    st(ctrl, "position", &d->pos);
    st(ctrl, "active", &d->active);
    d->state = label(body, UI_F_XL, UI_COL_TEXT, "-");
    d->bar = lv_bar_create(body);
    lv_obj_set_size(d->bar, LV_PCT(80), ui_px(14));
    lv_bar_set_range(d->bar, 0, 100);
    lv_obj_set_style_bg_color(d->bar, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_set_style_bg_color(d->bar, lv_color_hex(UI_COL_ON), LV_PART_INDICATOR);

    static const char *const names[] = { "Öffnen", "Stopp", "Schliessen" };
    static const char *const cmds[] = { "open", "stop", "close" };
    lv_obj_t *r = row(body);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = btn(r, names[i], ui_px(160));
        lv_obj_set_user_data(b, (void *)cmds[i]);
        lv_obj_add_event_cb(b, cmd_cb, LV_EVENT_CLICKED, d);
    }
    lv_obj_t *r2 = row(body);
    lv_obj_t *p = btn(r2, "Teilweise öffnen", ui_px(250));
    lv_obj_set_user_data(p, (void *)"partiallyOpen");
    lv_obj_add_event_cb(p, cmd_cb, LV_EVENT_CLICKED, d);
}

static void gate_update(ui_door_t *d)
{
    double pos = 0, act = 0;
    if (!val(&d->pos, &pos)) return;
    val(&d->active, &act);
    char b[32];
    ui_gate_text(pos, act, b, sizeof(b));
    lv_label_set_text(d->state, b);
    lv_obj_set_style_text_color(d->state, lv_color_hex(pos > 0.01 || fabs(act) > 0.5 ? UI_COL_WARN : UI_COL_TEXT), 0);
    lv_bar_set_value(d->bar, (int)lround(pos * 100), LV_ANIM_OFF);
}

/* ---------------- NFC Code Touch ---------------- */

static SemaphoreHandle_t s_mx;
static char *s_hist;
static uint32_t s_hist_gen;
static uint32_t s_gen_counter;

static void hist_cb(void *ctx, int code, const char *value)
{
    if (code != 200 || value == NULL) {
        if (code) ESP_LOGW(TAG, "Verlauf: Code %d", code);
        return;
    }
    size_t n = strlen(value);
    char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (copy == NULL) return;
    memcpy(copy, value, n + 1);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    heap_caps_free(s_hist);
    s_hist = copy;
    s_hist_gen = (uint32_t)(uintptr_t)ctx;
    xSemaphoreGive(s_mx);
}

static void out_cb(lv_event_t *e)
{
    ui_door_t *d = lv_event_get_user_data(e);
    int nr = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "output/%d", nr);
    send(d, cmd);
}

static const char *hist_type_text(int type)
{
    switch (type) {
    case 0: case 1: case 2: return "Code";
    case 3: case 4: case 5: return "NFC";
    case 6:  return "App";
    case 7:  return "Code abgelehnt";
    case 8:  return "NFC abgelehnt";
    case 9:  return "App abgelehnt";
    case 10: return "extern";
    case 11: return "extern abgelehnt";
    default: return "";
    }
}

static int cmp_ts_desc(const void *a, const void *b)
{
    double ta = cJSON_GetNumberValue(cJSON_GetObjectItem(*(const cJSON *const *)a, "ts"));
    double tb = cJSON_GetNumberValue(cJSON_GetObjectItem(*(const cJSON *const *)b, "ts"));
    return ta < tb ? 1 : ta > tb ? -1 : 0;
}

static void nfc_show_history(ui_door_t *d, const char *txt)
{
    cJSON *j = cJSON_Parse(txt);
    if (!cJSON_IsArray(j)) {
        cJSON_Delete(j);
        return;
    }
    lv_obj_clean(d->hist_box);
    int n = cJSON_GetArraySize(j);
    const cJSON **items = n ? malloc(n * sizeof(*items)) : NULL;
    if (n && items == NULL) {
        cJSON_Delete(j);
        return;
    }
    for (int i = 0; i < n; i++) items[i] = cJSON_GetArrayItem(j, i);
    qsort(items, n, sizeof(*items), cmp_ts_desc);
    if (n == 0) label(d->hist_box, UI_F_S, UI_COL_DIM, "Noch keine Zutritte");
    for (int i = 0; i < n && i < MAX_HIST; i++) {
        const cJSON *it = items[i];
        double ts = cJSON_GetNumberValue(cJSON_GetObjectItem(it, "ts"));
        if (ts > 1e11) ts /= 1000;      // Millisekunden
        time_t t = (time_t)ts;
        struct tm tm;
        localtime_r(&t, &tm);
        int type = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "type"));
        int output = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "output"));
        const cJSON *user = cJSON_GetObjectItem(it, "user");
        const cJSON *desc = cJSON_GetObjectItem(it, "description");
        const char *who = cJSON_IsString(user) && user->valuestring[0] ? user->valuestring
                        : cJSON_IsString(desc) && desc->valuestring[0] ? desc->valuestring : "";
        bool denied = output < 0 || type == 7 || type == 8 || type == 9 || type == 11;
        char b[160];
        snprintf(b, sizeof(b), "%02d.%02d. %02d:%02d   %s%s%s", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min,
                 who, who[0] ? " - " : "", hist_type_text(type));
        lv_obj_t *l = label(d->hist_box, UI_F_S, denied ? UI_COL_ERR : UI_COL_TEXT, b);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    free(items);
    cJSON_Delete(j);
}

static void nfc_create(ui_door_t *d, lv_obj_t *body, const cJSON *ctrl)
{
    if (s_mx == NULL) s_mx = xSemaphoreCreateMutex();
    st(ctrl, "historyDate", &d->hist_date);

    lv_obj_t *r = row(body);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, ui_px(10), 0);
    const cJSON *outs = cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "details"), "accessOutputs");
    const cJSON *o;
    cJSON_ArrayForEach(o, outs) {
        if (d->nout >= MAX_OUT || !cJSON_IsString(o) || o->string == NULL) continue;
        int nr = atoi(o->string + (o->string[0] == 'q' || o->string[0] == 'Q' ? 1 : 0));
        if (nr < 1 || nr > 6) continue;
        d->out_nr[d->nout++] = nr;
        lv_obj_t *b = btn(r, o->valuestring[0] ? o->valuestring : "Ausgang", ui_px(200));
        lv_obj_set_user_data(b, (void *)(intptr_t)nr);
        lv_obj_add_event_cb(b, out_cb, LV_EVENT_CLICKED, d);
    }
    if (d->nout == 0) label(r, UI_F_S, UI_COL_DIM, "Keine Ausgänge freigegeben");

    lv_obj_t *h = label(body, UI_F_S, UI_COL_DIM, "Letzte Zutritte");
    lv_obj_set_width(h, LV_PCT(100));
    d->hist_box = lv_obj_create(body);
    lv_obj_remove_style_all(d->hist_box);
    lv_obj_set_width(d->hist_box, LV_PCT(100));
    lv_obj_set_flex_grow(d->hist_box, 1);
    lv_obj_set_flex_flow(d->hist_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(d->hist_box, ui_px(6), 0);
    label(d->hist_box, UI_F_S, UI_COL_DIM, "lädt ...");
    d->gen = ++s_gen_counter;
    lox_query(d->uuid, "history", hist_cb, (void *)(uintptr_t)d->gen);
}

static void nfc_update(ui_door_t *d)
{
    /* neuer Eintrag im Verlauf -> neu laden */
    double hd;
    if (val(&d->hist_date, &hd)) {
        if (d->hist_seen_ok && hd != d->hist_seen) {
            d->gen = ++s_gen_counter;
            lox_query(d->uuid, "history", hist_cb, (void *)(uintptr_t)d->gen);
        }
        d->hist_seen = hd;
        d->hist_seen_ok = true;
    }
    char *txt = NULL;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_hist) {
        if (s_hist_gen == d->gen) txt = s_hist;
        else heap_caps_free(s_hist);
        s_hist = NULL;
    }
    xSemaphoreGive(s_mx);
    if (txt) {
        nfc_show_history(d, txt);
        heap_caps_free(txt);
    }
}

/* ---------------- Fenster-/Türüberwachung ---------------- */

bool ui_winmon_text(double open, double tilted, double unlocked, double offline, char *out, size_t size)
{
    out[0] = '\0';
    char *p = out;
    size_t left = size;
    struct { double n; const char *t; } parts[] = {
        { open, "offen" }, { tilted, "gekippt" }, { unlocked, "unverriegelt" }, { offline, "offline" },
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (parts[i].n < 0.5) continue;
        int w = snprintf(p, left, "%s%d %s", p == out ? "" : ", ", (int)lround(parts[i].n), parts[i].t);
        if (w < 0 || (size_t)w >= left) break;
        p += w;
        left -= w;
    }
    if (out[0] == '\0') {
        strlcpy(out, "Alles zu", size);
        return false;
    }
    return true;
}

static void win_state_text(int m, char *out, size_t size, uint32_t *col)
{
    if (m == 0) { strlcpy(out, "unbekannt", size); *col = UI_COL_DIM; }
    else if (m & 4) { strlcpy(out, "Offen", size); *col = UI_COL_WARN; }
    else if (m & 2) { strlcpy(out, "Gekippt", size); *col = UI_COL_WARN; }
    else if (m & 16) { strlcpy(out, "Zu, nicht verriegelt", size); *col = UI_COL_WARN; }
    else if (m & 8) { strlcpy(out, "Verriegelt", size); *col = UI_COL_ON; }
    else { strlcpy(out, "Zu", size); *col = UI_COL_ON; }
}

static void win_create(ui_door_t *d, lv_obj_t *body, const cJSON *ctrl)
{
    st(ctrl, "windowStates", &d->win_states);
    lv_obj_t *list = lv_obj_create(body);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, ui_px(8), 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    const cJSON *w;
    cJSON_ArrayForEach(w, cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "details"), "windows")) {
        if (d->nwin >= MAX_WIN) break;
        const cJSON *n = cJSON_GetObjectItem(w, "name");
        const cJSON *ip = cJSON_GetObjectItem(w, "installPlace");
        const char *rn = lox_struct_room_of(w);
        char t[128];
        snprintf(t, sizeof(t), "%s%s%s%s%s", cJSON_IsString(n) ? n->valuestring : "?",
                 cJSON_IsString(ip) && ip->valuestring[0] ? " (" : "",
                 cJSON_IsString(ip) && ip->valuestring[0] ? ip->valuestring : "",
                 cJSON_IsString(ip) && ip->valuestring[0] ? ")" : "", "");
        lv_obj_t *r = lv_obj_create(list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(r, lv_color_hex(UI_COL_TILE), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, ui_px(10), 0);
        lv_obj_set_style_pad_all(r, ui_px(10), 0);
        lv_obj_t *nl = label(r, UI_F_S, UI_COL_TEXT, t);
        lv_obj_set_width(nl, LV_PCT(62));
        lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
        if (rn) {
            lv_obj_t *rl = label(r, UI_F_S, UI_COL_DIM, rn);
            lv_obj_set_width(rl, LV_PCT(62));
            lv_label_set_long_mode(rl, LV_LABEL_LONG_DOT);
            lv_obj_align(rl, LV_ALIGN_TOP_LEFT, 0, ui_px(22));
        }
        d->win_state[d->nwin] = label(r, UI_F_M, UI_COL_DIM, "-");
        lv_obj_align(d->win_state[d->nwin], LV_ALIGN_RIGHT_MID, 0, 0);
        d->nwin++;
    }
    if (d->nwin == 0) label(list, UI_F_S, UI_COL_DIM, "Keine Fenster oder Türen hinterlegt");
}

static void win_update(ui_door_t *d)
{
    char txt[256];
    if (!d->win_states.ok || !lox_text_get(&d->win_states.u, txt, sizeof(txt))) return;
    uint32_t h = 5381;
    for (const char *p = txt; *p; p++) h = h * 33 + (uint8_t)*p;
    if (h == d->win_hash) return;
    d->win_hash = h;
    const char *p = txt;
    for (int i = 0; i < d->nwin; i++) {
        int m = 0;
        if (p && *p) {
            m = atoi(p);
            p = strchr(p, ',');
            if (p) p++;
        }
        char b[40];
        uint32_t col;
        win_state_text(m, b, sizeof(b), &col);
        lv_label_set_text(d->win_state[i], b);
        lv_obj_set_style_text_color(d->win_state[i], lv_color_hex(col), 0);
    }
}

/* ---------------- allgemein ---------------- */

ui_door_t *ui_door_detail_create(lv_obj_t *parent, const cJSON *ctrl, const char *room)
{
    const cJSON *t = cJSON_GetObjectItem(ctrl, "type");
    if (!cJSON_IsString(t) || !ui_door_supports(t->valuestring)) return NULL;
    ui_door_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM);
    if (d == NULL) return NULL;
    d->kind = !strcmp(t->valuestring, "Gate") ? D_GATE : !strcmp(t->valuestring, "NfcCodeTouch") ? D_NFC : D_WIN;
    const cJSON *u = cJSON_GetObjectItem(ctrl, "uuidAction");
    const cJSON *n = cJSON_GetObjectItem(ctrl, "name");
    strlcpy(d->uuid, cJSON_IsString(u) ? u->valuestring : "", sizeof(d->uuid));
    strlcpy(d->name, cJSON_IsString(n) ? n->valuestring : "?", sizeof(d->name));
    d->secured = cJSON_IsTrue(cJSON_GetObjectItem(ctrl, "isSecured"));

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, ui_px(14), 0);

    lv_obj_t *nl = label(parent, UI_F_L, UI_COL_TEXT, d->name);
    lv_obj_set_width(nl, LV_PCT(100));
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_t *rl = label(parent, UI_F_S, UI_COL_DIM, "");
    lv_label_set_text_fmt(rl, "%s%s", room ? room : "", d->secured ? "  (gesichert - Bedienung nur in der App)" : "");

    switch (d->kind) {
    case D_GATE: gate_create(d, parent, ctrl); break;
    case D_NFC:  nfc_create(d, parent, ctrl); break;
    default:     win_create(d, parent, ctrl); break;
    }
    ui_door_update(d);
    return d;
}

void ui_door_update(ui_door_t *d)
{
    if (d == NULL) return;
    switch (d->kind) {
    case D_GATE: gate_update(d); break;
    case D_NFC:  nfc_update(d); break;
    default:     win_update(d); break;
    }
}

void ui_door_free(ui_door_t *d)
{
    heap_caps_free(d);
}

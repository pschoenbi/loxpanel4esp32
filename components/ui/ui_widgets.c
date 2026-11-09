/*
 * Widgets für Schalter/Taster, Dimmer und Jalousie.
 *
 * Loxone-Befehle (jdev/sps/io/<uuidAction>/<cmd>):
 *   Switch:      on, off            Pushbutton: pulse
 *   Dimmer:      on, off, <wert>    (Status: position, min, max)
 *   Jalousie:    FullUp/FullDown (Tippen), up/UpOff, down/DownOff (Halten), stop
 *                (Status: position 0 = oben/offen ... 1 = unten/zu, up, down = fährt)
 *   LightControllerV2: changeTo/<moodId>, plus, minus; Stimmung 778 = "Aus"
 *                (Text-Status: activeMoods "[1,2]", moodList [{"name","id",...}])
 *                Unterbaustein ".../masterValue" (Dimmer) = Master-Helligkeit
 */

#include "ui_widgets.h"
#include "ui_cam.h"
#include "ui_door.h"
#include "ui_efm.h"
#include "ui_irc.h"
#include "ui_priv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lox_client.h"
#include "lox_struct.h"

static const char *TAG = "ui_widget";

typedef enum { W_SWITCH, W_DIMMER, W_JALOUSIE, W_LIGHT, W_IRC, W_EFM, W_CAM, W_DOOR } wkind_t;

typedef struct {
    lox_uuid_t uuid;
    bool valid;
} state_t;

#define MAX_MOODS   24

struct ui_widget {
    wkind_t kind;
    bool pushbutton;
    bool secured;
    char uuid[48];              // uuidAction
    char name[48];
    state_t st_active, st_pos, st_min, st_max, st_up, st_down;
    lv_obj_t *root;
    lv_obj_t *btn, *btn_icon, *btn_text;    // Schalter / Dimmer ein-aus
    lv_obj_t *value;                        // Dimmer-Wert, Jalousie-Position
    lv_obj_t *slider;
    lv_obj_t *bar;
    lv_obj_t *moving;
    lv_obj_t *jbtn[3];                      // auf, stopp, ab
    bool dragging;
    bool held[3];
    /* Lichtsteuerung */
    state_t st_moods, st_list;
    char master_uuid[64];
    state_t m_pos, m_min, m_max;
    lv_obj_t *mood_box;
    lv_obj_t *mood_btn[MAX_MOODS];
    int mood_id[MAX_MOODS];
    int mood_count;
    uint32_t list_hash;
    /* Raumregler */
    state_t r_act, r_target, r_comfort, r_offset, r_amode, r_opmode, r_co2, r_hum;
    char fmt[24];
    lv_obj_t *r_info, *r_op[4];
    ui_irc_t *irc;
    ui_efm_t *efm;
    ui_cam_t *cam;
    ui_door_t *door;
};

bool ui_widget_supports(const char *t)
{
    return t && (!strcmp(t, "Switch") || !strcmp(t, "Pushbutton") || !strcmp(t, "Dimmer") ||
                 !strcmp(t, "EIBDimmer") || !strcmp(t, "Jalousie") || !strcmp(t, "LightControllerV2") ||
                 !strcmp(t, "IRoomControllerV2") || !strcmp(t, "EFM") ||
                 !strcmp(t, "Intercom") || !strcmp(t, "IntercomV2") || ui_door_supports(t));
}

/* ---------- Lichtstimmungen (auch für die kompakte Kachel) ---------- */

int ui_light_active_moods(const lox_uuid_t *active_state, int *ids, int max)
{
    char buf[160];
    if (!lox_text_get(active_state, buf, sizeof(buf))) return -1;
    cJSON *a = cJSON_Parse(buf);
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, a) {
        if (cJSON_IsNumber(e) && n < max) ids[n++] = e->valueint;
    }
    cJSON_Delete(a);
    return n;
}

bool ui_light_is_off(const int *ids, int n)
{
    return n <= 0 || (n == 1 && ids[0] == UI_MOOD_OFF);
}

/* Namen der aktiven Stimmungen, z. B. "Hell + Abend" */
void ui_light_mood_text(const lox_uuid_t *list_state, const int *ids, int n, char *out, size_t size)
{
    out[0] = '\0';
    if (ui_light_is_off(ids, n)) {
        strlcpy(out, "Aus", size);
        return;
    }
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    cJSON *list = (buf && lox_text_get(list_state, buf, 4096)) ? cJSON_Parse(buf) : NULL;
    heap_caps_free(buf);
    for (int i = 0; i < n; i++) {
        const char *name = NULL;
        const cJSON *m;
        cJSON_ArrayForEach(m, list) {
            const cJSON *id = cJSON_GetObjectItem(m, "id");
            const cJSON *nm = cJSON_GetObjectItem(m, "name");
            if (cJSON_IsNumber(id) && id->valueint == ids[i] && cJSON_IsString(nm)) name = nm->valuestring;
        }
        char tmp[16];
        if (name == NULL) {
            snprintf(tmp, sizeof(tmp), "Stimmung %d", ids[i]);
            name = tmp;
        }
        if (out[0]) strlcat(out, " + ", size);
        strlcat(out, name, size);
    }
    cJSON_Delete(list);
}

static void get_state(const cJSON *ctrl, const char *name, state_t *out)
{
    const cJSON *states = cJSON_GetObjectItem(ctrl, "states");
    const cJSON *s = states ? cJSON_GetObjectItem(states, name) : NULL;
    out->valid = cJSON_IsString(s) && lox_uuid_parse(s->valuestring, &out->uuid);
}

static bool read(const state_t *s, double *v)
{
    return s->valid && lox_value_get(&s->uuid, v);
}

static void send(ui_widget_t *w, const char *cmd)
{
    if (w->secured) {
        ESP_LOGW(TAG, "Baustein ist gesichert (Visualisierungspasswort) - Befehl nicht gesendet");
        return;
    }
    ESP_LOGI(TAG, "%s: %s", w->name, cmd);
    if (lox_send_command(w->uuid, cmd) != ESP_OK) {
        ESP_LOGW(TAG, "Befehl %s nicht gesendet (Miniserver nicht online)", cmd);
    }
}

static lv_obj_t *round_button(lv_obj_t *parent, int size, const char *symbol)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_40, 0);
    lv_obj_center(l);
    return b;
}

/* ---------- Schalter ---------- */

static void switch_clicked(lv_event_t *e)
{
    ui_widget_t *w = lv_event_get_user_data(e);
    if (w->pushbutton) {
        send(w, "pulse");
        return;
    }
    double v = 0;
    read(&w->st_active, &v);
    send(w, v > 0.5 ? "off" : "on");
}

static void switch_create(ui_widget_t *w)
{
    w->btn = round_button(w->root, ui_px(150), LV_SYMBOL_POWER);
    lv_obj_add_event_cb(w->btn, switch_clicked, LV_EVENT_CLICKED, w);
    w->btn_text = lv_label_create(w->root);
    lv_obj_set_style_text_font(w->btn_text, ui_font(UI_F_M), 0);
    lv_label_set_text(w->btn_text, w->pushbutton ? "Taster" : "-");
}

static void switch_update(ui_widget_t *w)
{
    double v;
    bool known = read(&w->st_active, &v);
    bool on = known && v > 0.5;
    lv_obj_set_style_bg_color(w->btn, lv_color_hex(on ? UI_COL_ON : UI_COL_BTN), 0);
    if (!w->pushbutton) {
        lv_label_set_text(w->btn_text, !known ? "-" : on ? "Ein" : "Aus");
    }
}

/* ---------- Dimmer ---------- */

static void dimmer_range(ui_widget_t *w, double *mn, double *mx)
{
    *mn = 0;
    *mx = 100;
    read(&w->st_min, mn);
    read(&w->st_max, mx);
    if (*mx <= *mn) *mx = *mn + 100;
}

static void dimmer_slider_event(lv_event_t *e)
{
    ui_widget_t *w = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    int32_t v = lv_slider_get_value(w->slider);
    if (code == LV_EVENT_PRESSED) {
        w->dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text_fmt(w->value, "%d %%", (int)v);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        w->dragging = false;
        double mn, mx;
        dimmer_range(w, &mn, &mx);
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "%d", (int)lround(mn + (mx - mn) * v / 100.0));
        send(w, cmd);
    }
}

static void dimmer_toggle(lv_event_t *e)
{
    ui_widget_t *w = lv_event_get_user_data(e);
    double pos = 0, mn, mx;
    dimmer_range(w, &mn, &mx);
    read(&w->st_pos, &pos);
    send(w, pos > mn ? "off" : "on");
}

static void dimmer_create(ui_widget_t *w)
{
    w->value = lv_label_create(w->root);
    lv_obj_set_style_text_font(w->value, ui_font(UI_F_XL), 0);
    lv_label_set_text(w->value, "- %");

    w->slider = lv_slider_create(w->root);
    lv_obj_set_size(w->slider, LV_PCT(85), ui_px(36));
    lv_slider_set_range(w->slider, 0, 100);
    lv_obj_add_event_cb(w->slider, dimmer_slider_event, LV_EVENT_ALL, w);

    w->btn = round_button(w->root, ui_px(90), LV_SYMBOL_POWER);
    lv_obj_add_event_cb(w->btn, dimmer_toggle, LV_EVENT_CLICKED, w);
}

static void dimmer_update(ui_widget_t *w)
{
    double pos, mn, mx;
    dimmer_range(w, &mn, &mx);
    bool known = read(&w->st_pos, &pos);
    lv_obj_set_style_bg_color(w->btn, lv_color_hex(known && pos > mn ? UI_COL_ON : UI_COL_BTN), 0);
    if (!known || w->dragging) return;
    int pct = (int)lround((pos - mn) * 100.0 / (mx - mn));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_slider_set_value(w->slider, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(w->value, "%d %%", pct);
}

/* ---------- Jalousie ---------- */

/* Tippen = ganz auf/zu, Halten = fahren solange gedrückt */
static void jal_event(lv_event_t *e)
{
    ui_widget_t *w = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    int i = btn == w->jbtn[0] ? 0 : btn == w->jbtn[1] ? 1 : 2;

    if (i == 1) {
        if (code == LV_EVENT_CLICKED) send(w, "stop");
        return;
    }
    const char *hold = i == 0 ? "up" : "down";
    const char *off  = i == 0 ? "UpOff" : "DownOff";
    const char *full = i == 0 ? "FullUp" : "FullDown";

    if (code == LV_EVENT_LONG_PRESSED) {
        w->held[i] = true;
        send(w, hold);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (w->held[i]) {
            w->held[i] = false;
            send(w, off);
        } else if (code == LV_EVENT_RELEASED) {
            send(w, full);
        }
    }
}

static void jalousie_create(ui_widget_t *w)
{
    w->value = lv_label_create(w->root);
    lv_obj_set_style_text_font(w->value, ui_font(UI_F_L), 0);
    lv_label_set_text(w->value, "-");

    w->bar = lv_bar_create(w->root);
    lv_obj_set_size(w->bar, LV_PCT(85), ui_px(18));
    lv_bar_set_range(w->bar, 0, 100);

    w->moving = lv_label_create(w->root);
    lv_obj_set_style_text_font(w->moving, ui_font(UI_F_S), 0);
    lv_obj_set_style_text_color(w->moving, lv_color_hex(UI_COL_WARN), 0);
    lv_label_set_text(w->moving, "");

    lv_obj_t *row = lv_obj_create(w->root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *sym[3] = { LV_SYMBOL_UP, LV_SYMBOL_STOP, LV_SYMBOL_DOWN };
    for (int i = 0; i < 3; i++) {
        w->jbtn[i] = round_button(row, ui_px(84), sym[i]);
        lv_obj_add_event_cb(w->jbtn[i], jal_event, LV_EVENT_ALL, w);
    }

    lv_obj_t *hint = lv_label_create(w->root);
    lv_obj_set_style_text_font(hint, ui_font(UI_F_S), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COL_DIM), 0);
    lv_label_set_text(hint, "Tippen: ganz  -  Halten: Schritt");
}

static void jalousie_update(ui_widget_t *w)
{
    double pos, up = 0, down = 0;
    if (read(&w->st_pos, &pos)) {
        int pct = (int)lround(pos * 100.0);
        lv_bar_set_value(w->bar, pct, LV_ANIM_OFF);
        if (pct <= 0)        lv_label_set_text(w->value, "Offen");
        else if (pct >= 100) lv_label_set_text(w->value, "Geschlossen");
        else                 lv_label_set_text_fmt(w->value, "%d %% zu", pct);
    }
    read(&w->st_up, &up);
    read(&w->st_down, &down);
    lv_label_set_text(w->moving, up > 0.5 ? "fährt hoch" : down > 0.5 ? "fährt runter" : "");
}

/* ---------- Lichtsteuerung ---------- */

static void mood_clicked(lv_event_t *e)
{
    ui_widget_t *w = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    for (int i = 0; i < w->mood_count; i++) {
        if (w->mood_btn[i] == b) {
            char cmd[24];
            snprintf(cmd, sizeof(cmd), "changeTo/%d", w->mood_id[i]);
            send(w, cmd);
        }
    }
}

static void master_event(lv_event_t *e)
{
    ui_widget_t *w = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        w->dragging = true;
    } else if (c == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text_fmt(w->value, "Helligkeit %d %%", (int)lv_slider_get_value(w->slider));
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        w->dragging = false;
        double mn = 0, mx = 100;
        read(&w->m_min, &mn);
        read(&w->m_max, &mx);
        if (mx <= mn) mx = mn + 100;
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "%d", (int)lround(mn + (mx - mn) * lv_slider_get_value(w->slider) / 100.0));
        ESP_LOGI(TAG, "%s Master: %s", w->name, cmd);
        lox_send_command(w->master_uuid, cmd);
    }
}

static uint32_t str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* Knöpfe aus moodList neu aufbauen, wenn sich die Liste geändert hat */
static void light_build_moods(ui_widget_t *w)
{
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (buf == NULL) return;
    if (!lox_text_get(&w->st_list.uuid, buf, 4096)) {
        heap_caps_free(buf);
        return;
    }
    uint32_t h = str_hash(buf);
    if (h == w->list_hash) {
        heap_caps_free(buf);
        return;
    }
    w->list_hash = h;
    cJSON *list = cJSON_Parse(buf);
    heap_caps_free(buf);

    lv_obj_clean(w->mood_box);
    w->mood_count = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, list) {
        const cJSON *id = cJSON_GetObjectItem(m, "id");
        const cJSON *nm = cJSON_GetObjectItem(m, "name");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(nm) || w->mood_count >= MAX_MOODS) continue;
        lv_obj_t *b = lv_button_create(w->mood_box);
        lv_obj_set_height(b, ui_px(56));
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_BTN), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_ON), LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_hor(b, ui_px(18), 0);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, ui_font(UI_F_S), 0);
        lv_label_set_text(l, nm->valuestring);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, mood_clicked, LV_EVENT_CLICKED, w);
        w->mood_btn[w->mood_count] = b;
        w->mood_id[w->mood_count] = id->valueint;
        w->mood_count++;
    }
    cJSON_Delete(list);
}

static void light_create(ui_widget_t *w, const cJSON *ctrl)
{
    get_state(ctrl, "activeMoods", &w->st_moods);
    get_state(ctrl, "moodList", &w->st_list);

    /* Master-Helligkeit: Unterbaustein ".../masterValue" */
    const cJSON *sc;
    cJSON_ArrayForEach(sc, cJSON_GetObjectItem(ctrl, "subControls")) {
        const cJSON *u = cJSON_GetObjectItem(sc, "uuidAction");
        if (cJSON_IsString(u) && strstr(u->valuestring, "/masterValue")) {
            strlcpy(w->master_uuid, u->valuestring, sizeof(w->master_uuid));
            get_state(sc, "position", &w->m_pos);
            get_state(sc, "min", &w->m_min);
            get_state(sc, "max", &w->m_max);
        }
    }

    w->btn_text = lv_label_create(w->root);
    lv_obj_set_style_text_font(w->btn_text, ui_font(UI_F_L), 0);
    lv_obj_set_style_text_color(w->btn_text, lv_color_hex(UI_COL_ON), 0);
    lv_label_set_text(w->btn_text, "-");

    w->mood_box = lv_obj_create(w->root);
    lv_obj_remove_style_all(w->mood_box);
    lv_obj_set_size(w->mood_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(w->mood_box, ui_px(220), 0);
    lv_obj_set_flex_flow(w->mood_box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(w->mood_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(w->mood_box, ui_px(10), 0);
    lv_obj_set_style_pad_column(w->mood_box, ui_px(10), 0);

    if (w->master_uuid[0]) {
        w->value = lv_label_create(w->root);
        lv_obj_set_style_text_font(w->value, ui_font(UI_F_S), 0);
        lv_label_set_text(w->value, "Helligkeit -");
        w->slider = lv_slider_create(w->root);
        lv_obj_set_size(w->slider, LV_PCT(85), ui_px(30));
        lv_slider_set_range(w->slider, 0, 100);
        lv_obj_add_event_cb(w->slider, master_event, LV_EVENT_ALL, w);
    }
}

static void light_update(ui_widget_t *w)
{
    if (w->st_list.valid) light_build_moods(w);
    int ids[8];
    int n = w->st_moods.valid ? ui_light_active_moods(&w->st_moods.uuid, ids, 8) : -1;
    if (n >= 0) {
        char txt[96];
        ui_light_mood_text(&w->st_list.uuid, ids, n, txt, sizeof(txt));
        lv_label_set_text(w->btn_text, txt);
        lv_obj_set_style_text_color(w->btn_text, lv_color_hex(ui_light_is_off(ids, n) ? UI_COL_DIM : UI_COL_ON), 0);
        for (int i = 0; i < w->mood_count; i++) {
            bool on = false;
            for (int k = 0; k < n; k++) on |= ids[k] == w->mood_id[i];
            if (on) lv_obj_add_state(w->mood_btn[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(w->mood_btn[i], LV_STATE_CHECKED);
        }
    }
    double pos, mn = 0, mx = 100;
    if (w->slider && !w->dragging && read(&w->m_pos, &pos)) {
        read(&w->m_min, &mn);
        read(&w->m_max, &mx);
        if (mx <= mn) mx = mn + 100;
        int pct = (int)lround((pos - mn) * 100.0 / (mx - mn));
        pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        lv_slider_set_value(w->slider, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(w->value, "Helligkeit %d %%", pct);
    }
}

/* ---------- allgemein ---------- */

ui_widget_t *ui_widget_create(lv_obj_t *parent, const cJSON *ctrl, const char *room)
{
    const cJSON *type = cJSON_GetObjectItem(ctrl, "type");
    const cJSON *name = cJSON_GetObjectItem(ctrl, "name");
    const cJSON *uuid = cJSON_GetObjectItem(ctrl, "uuidAction");
    if (!cJSON_IsString(type) || !cJSON_IsString(uuid) || !ui_widget_supports(type->valuestring)) {
        return NULL;
    }
    ui_widget_t *w = calloc(1, sizeof(*w));
    if (w == NULL) return NULL;
    strlcpy(w->uuid, uuid->valuestring, sizeof(w->uuid));
    strlcpy(w->name, cJSON_IsString(name) ? name->valuestring : "?", sizeof(w->name));
    w->secured = cJSON_IsTrue(cJSON_GetObjectItem(ctrl, "isSecured"));

    const char *t = type->valuestring;
    if (ui_door_supports(t)) {
        w->kind = W_DOOR;
    } else if (!strcmp(t, "Intercom") || !strcmp(t, "IntercomV2")) {
        w->kind = W_CAM;
    } else if (!strcmp(t, "EFM")) {
        w->kind = W_EFM;
    } else if (!strcmp(t, "IRoomControllerV2")) {
        w->kind = W_IRC;
    } else if (!strcmp(t, "LightControllerV2")) {
        w->kind = W_LIGHT;
    } else if (!strcmp(t, "Jalousie")) {
        w->kind = W_JALOUSIE;
        get_state(ctrl, "position", &w->st_pos);
        get_state(ctrl, "up", &w->st_up);
        get_state(ctrl, "down", &w->st_down);
    } else if (!strcmp(t, "Dimmer") || !strcmp(t, "EIBDimmer")) {
        w->kind = W_DIMMER;
        get_state(ctrl, "position", &w->st_pos);
        get_state(ctrl, "min", &w->st_min);
        get_state(ctrl, "max", &w->st_max);
    } else {
        w->kind = W_SWITCH;
        w->pushbutton = !strcmp(t, "Pushbutton");
        get_state(ctrl, "active", &w->st_active);
    }

    w->root = lv_obj_create(parent);
    lv_obj_remove_style_all(w->root);
    lv_obj_set_size(w->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(w->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w->root, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(w->root, LV_OBJ_FLAG_SCROLLABLE);

    /* Raumregler: eigene Detailansicht (ui_irc.c) */
    if (w->kind == W_IRC) {
        lv_obj_set_layout(w->root, LV_LAYOUT_NONE);
        w->irc = ui_irc_detail_create(w->root, ctrl, room);
        return w;
    }
    if (w->kind == W_EFM) {
        lv_obj_set_layout(w->root, LV_LAYOUT_NONE);
        w->efm = ui_efm_detail_create(w->root, ctrl, room);
        return w;
    }
    if (w->kind == W_CAM) {
        lv_obj_set_layout(w->root, LV_LAYOUT_NONE);
        w->cam = ui_cam_detail_create(w->root, ctrl, room);
        return w;
    }
    if (w->kind == W_DOOR) {
        w->door = ui_door_detail_create(w->root, ctrl, room);
        return w;
    }

    lv_obj_t *n = lv_label_create(w->root);
    lv_obj_set_style_text_font(n, ui_font(UI_F_L), 0);
    lv_obj_set_width(n, LV_PCT(100));
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_label_set_text(n, cJSON_IsString(name) ? name->valuestring : "?");

    lv_obj_t *r = lv_label_create(w->root);
    lv_obj_set_style_text_font(r, ui_font(UI_F_S), 0);
    lv_obj_set_style_text_color(r, lv_color_hex(UI_COL_DIM), 0);
    lv_label_set_text_fmt(r, "%s%s", room ? room : "", w->secured ? "  (gesichert)" : "");

    switch (w->kind) {
    case W_SWITCH:   switch_create(w); break;
    case W_DIMMER:   dimmer_create(w); break;
    case W_JALOUSIE: jalousie_create(w); break;
    case W_LIGHT:    light_create(w, ctrl); break;
    case W_IRC:      break;
    case W_EFM:      break;
    case W_CAM:      break;
    case W_DOOR:     break;
    }
    ui_widget_update(w);
    return w;
}

void ui_widget_update(ui_widget_t *w)
{
    if (w == NULL) return;
    switch (w->kind) {
    case W_SWITCH:   switch_update(w); break;
    case W_DIMMER:   dimmer_update(w); break;
    case W_JALOUSIE: jalousie_update(w); break;
    case W_LIGHT:    light_update(w); break;
    case W_IRC:      ui_irc_update(w->irc); break;
    case W_EFM:      ui_efm_update(w->efm); break;
    case W_CAM:      ui_cam_update(w->cam); break;
    case W_DOOR:     ui_door_update(w->door); break;
    }
}

void ui_widget_destroy(ui_widget_t *w)
{
    if (w == NULL) return;
    ui_irc_free(w->irc);
    ui_efm_free(w->efm);
    ui_cam_free(w->cam);
    ui_door_free(w->door);
    lv_obj_delete(w->root);
    free(w);
}

/*
 * Türsprechstelle / Kamera.
 *
 * Laut Loxone "Structure File": Intercom (securedDetails.videoInfo: streamUrl, alertImage, user, pass;
 * Status bell; Befehl answer; Unterbausteine Pushbutton = Ausgänge wie Türöffner/Licht) und
 * IntercomV2 (ähnlich). streamUrl mit "cloudDNS"/"remoteConnect" (Fernzugriff) wird nicht unterstützt.
 */

#include "ui_cam.h"
#include "ui_priv.h"
#include "misc/cache/lv_cache.h"     // lv_image_cache_drop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cam.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lox_client.h"
#include "lox_struct.h"

static const char *TAG = "ui_cam";

#define MAX_OUTS 6

struct ui_cam {
    char uuid[48];
    char name[64];
    lox_uuid_t bell;
    bool bell_ok;
    int nouts;
    char out_uuid[MAX_OUTS][48];
    lv_obj_t *video, *img, *status, *bell_lbl, *answer;
    lv_timer_t *timer;
    lv_image_dsc_t dsc[2];
    int cur;
    int64_t last_frame_us;
    bool streaming;
    char msg[64];           // fester Hinweis (z. B. keine URL)
};

static lv_obj_t *label(lv_obj_t *parent, ui_font_id_t f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, ui_font(f), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *btn(lv_obj_t *parent, const char *txt, int w, int h)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_BTN), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, ui_px(12), 0);
    lv_obj_t *l = label(b, UI_F_S, UI_COL_TEXT, txt);
    lv_obj_set_width(l, w - ui_px(16));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

static void out_cb(lv_event_t *e)
{
    ui_cam_t *c = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= c->nouts) return;
    ESP_LOGI(TAG, "%s: Ausgang %d", c->name, i);
    lox_send_command(c->out_uuid[i], "pulse");
}

static void answer_cb(lv_event_t *e)
{
    ui_cam_t *c = lv_event_get_user_data(e);
    lox_send_command(c->uuid, "answer");
}

/* Neues Bild vom Kamera-Task übernehmen */
static void frame_timer(lv_timer_t *t)
{
    ui_cam_t *c = lv_timer_get_user_data(t);
    cam_frame_t f;
    if (cam_get_frame(&f)) {
        int n = c->cur ^ 1;
        lv_image_dsc_t *d = &c->dsc[n];
        lv_image_cache_drop(d);
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = f.w;
        d->header.h = f.h;
        d->header.stride = f.w * 2;
        d->data = f.pixels;
        d->data_size = (uint32_t)f.w * f.h * 2;
        lv_image_set_src(c->img, d);
        lv_obj_center(c->img);
        c->cur = n;
        c->last_frame_us = esp_timer_get_time();
        if (!c->streaming) {
            c->streaming = true;
            lv_obj_add_flag(c->status, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (c->streaming && esp_timer_get_time() - c->last_frame_us > 4000000) {
        c->streaming = false;
        lv_obj_remove_flag(c->status, LV_OBJ_FLAG_HIDDEN);
    }
    if (!c->streaming) {
        char s[64];
        if (c->msg[0]) strlcpy(s, c->msg, sizeof(s));
        else cam_status(s, sizeof(s));
        if (strcmp(lv_label_get_text(c->status), s)) lv_label_set_text(c->status, s);
    }
}

/* Video-URL und Zugangsdaten aus den gesicherten Details; startet den Stream */
static void start_stream(ui_cam_t *c, int vw, int vh)
{
    char *js = lox_secured_details(c->uuid);
    if (js == NULL) {
        strlcpy(c->msg, "Keine Video-Adresse vom Miniserver", sizeof(c->msg));
        return;
    }
    cJSON *j = cJSON_Parse(js);
    free(js);
    const cJSON *vi = cJSON_GetObjectItem(j, "videoInfo");
    const cJSON *url = cJSON_GetObjectItem(vi, "streamUrl");
    const cJSON *user = cJSON_GetObjectItem(vi, "user");
    const cJSON *pass = cJSON_GetObjectItem(vi, "pass");
    if (!cJSON_IsString(url) || !url->valuestring[0]) {
        strlcpy(c->msg, "Keine Video-Adresse hinterlegt", sizeof(c->msg));
    } else if (strstr(url->valuestring, "remoteConnect") || strstr(url->valuestring, "cloudDNS")) {
        strlcpy(c->msg, "Video nur über Fernzugriff erreichbar", sizeof(c->msg));
    } else {
        cam_start(url->valuestring, cJSON_IsString(user) ? user->valuestring : NULL,
                  cJSON_IsString(pass) ? pass->valuestring : NULL, vw, vh);
    }
    cJSON_Delete(j);
}

ui_cam_t *ui_cam_detail_create(lv_obj_t *parent, const cJSON *ctrl, const char *room)
{
    ui_cam_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM);
    if (c == NULL) return NULL;
    const cJSON *u = cJSON_GetObjectItem(ctrl, "uuidAction");
    const cJSON *n = cJSON_GetObjectItem(ctrl, "name");
    strlcpy(c->uuid, cJSON_IsString(u) ? u->valuestring : "", sizeof(c->uuid));
    strlcpy(c->name, cJSON_IsString(n) ? n->valuestring : "Kamera", sizeof(c->name));
    const cJSON *b = cJSON_GetObjectItem(cJSON_GetObjectItem(ctrl, "states"), "bell");
    c->bell_ok = cJSON_IsString(b) && lox_uuid_parse(b->valuestring, &c->bell);

    lv_obj_update_layout(parent);
    int W = lv_obj_get_content_width(parent);
    int H = lv_obj_get_content_height(parent);
    if (W < 200) W = 940;
    if (H < 200) H = 440;

    const int col_w = ui_px(200);
    const int top = ui_px(44);

    lv_obj_t *t = label(parent, UI_F_M, UI_COL_TEXT, c->name);
    lv_obj_set_size(t, W - col_w - ui_px(70), ui_px(30));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, 0, ui_px(4));
    (void)room;

    /* Video */
    int vw = W - col_w - ui_px(16), vh = H - top;
    c->video = lv_obj_create(parent);
    lv_obj_remove_style_all(c->video);
    lv_obj_remove_flag(c->video, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c->video, vw, vh);
    lv_obj_set_pos(c->video, 0, top);
    lv_obj_set_style_bg_color(c->video, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c->video, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c->video, ui_px(12), 0);
    lv_obj_set_style_clip_corner(c->video, true, 0);
    c->img = lv_image_create(c->video);
    lv_obj_center(c->img);
    c->status = label(c->video, UI_F_M, UI_COL_DIM, "Verbinde ...");
    lv_obj_center(c->status);

    /* Bedienung rechts */
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, col_w, vh);
    lv_obj_set_pos(col, W - col_w, top);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, ui_px(10), 0);

    c->bell_lbl = label(col, UI_F_M, UI_COL_WARN, LV_SYMBOL_BELL " Es klingelt");
    lv_obj_add_flag(c->bell_lbl, LV_OBJ_FLAG_HIDDEN);
    c->answer = btn(col, "Klingel quittieren", col_w, ui_px(60));
    lv_obj_add_event_cb(c->answer, answer_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_flag(c->answer, LV_OBJ_FLAG_HIDDEN);

    const cJSON *sc;
    cJSON_ArrayForEach(sc, cJSON_GetObjectItem(ctrl, "subControls")) {
        const cJSON *su = cJSON_GetObjectItem(sc, "uuidAction");
        const cJSON *sn = cJSON_GetObjectItem(sc, "name");
        if (!cJSON_IsString(su) || c->nouts >= MAX_OUTS) continue;
        int i = c->nouts++;
        strlcpy(c->out_uuid[i], su->valuestring, sizeof(c->out_uuid[i]));
        lv_obj_t *ob = btn(col, cJSON_IsString(sn) ? sn->valuestring : "Ausgang", col_w, ui_px(60));
        lv_obj_set_user_data(ob, (void *)(intptr_t)i);
        lv_obj_add_event_cb(ob, out_cb, LV_EVENT_CLICKED, c);
    }

    start_stream(c, vw, vh);
    c->timer = lv_timer_create(frame_timer, 20, c);
    ui_cam_update(c);
    return c;
}

void ui_cam_update(ui_cam_t *c)
{
    if (c == NULL) return;
    double v = 0;
    bool ringing = c->bell_ok && lox_value_get(&c->bell, &v) && v > 0.5;
    if (ringing) {
        lv_obj_remove_flag(c->bell_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(c->answer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->bell_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->answer, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_cam_free(ui_cam_t *c)
{
    if (c == NULL) return;
    cam_stop();
    if (c->timer) lv_timer_delete(c->timer);
    lv_image_cache_drop(&c->dsc[0]);
    lv_image_cache_drop(&c->dsc[1]);
    heap_caps_free(c);
}

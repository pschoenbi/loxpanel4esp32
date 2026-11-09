/*
 * Grundgerüst der Oberfläche: Schriften, Kopfzeile, Seiten, Aktualisierungs-Timer.
 */

#include "ui.h"
#include "ui_priv.h"

#include <stdio.h>
#include <time.h>
#include "dev_settings.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lox_client.h"
#include "net_time.h"
#include "net_wifi.h"
#include "panel_config.h"

static const char *TAG = "ui";

extern const uint8_t ttf_start[] asm("_binary_Montserrat_Medium_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_Montserrat_Medium_ttf_end");

static const lv_font_t *s_fonts[UI_F_COUNT];

static lv_obj_t *s_clock;
static lv_obj_t *s_room;
static lv_obj_t *s_title;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_lox_icon;

int ui_px(int v)
{
    return v * lv_display_get_vertical_resolution(NULL) / 600;
}

const lv_font_t *ui_font(ui_font_id_t id)
{
    return s_fonts[id];
}

static void fonts_init(void)
{
    /* Schriftgrössen bei 600 px Höhe; Symbole kommen aus der eingebauten Montserrat */
    static const int sizes[UI_F_COUNT] = { 16, 21, 28, 48, 104 };
    static const lv_font_t *symbol_fallback[UI_F_COUNT] = {
        &lv_font_montserrat_14, &lv_font_montserrat_20, &lv_font_montserrat_28, &lv_font_montserrat_40,
        &lv_font_montserrat_40,
    };
    for (int i = 0; i < UI_F_COUNT; i++) {
        lv_font_t *f = lv_tiny_ttf_create_data_ex(ttf_start, ttf_end - ttf_start, ui_px(sizes[i]),
                                                  LV_FONT_KERNING_NORMAL, 256);
        if (f) {
            f->fallback = symbol_fallback[i];
            s_fonts[i] = f;
        } else {
            ESP_LOGE(TAG, "Schrift %d px nicht erzeugt", sizes[i]);
            s_fonts[i] = symbol_fallback[i];
        }
    }
}

lv_obj_t *ui_tile_create(lv_obj_t *parent)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_set_style_bg_color(t, lv_color_hex(UI_COL_TILE), 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, ui_px(16), 0);
    lv_obj_set_style_pad_all(t, ui_px(14), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    return t;
}

/* ---------- Kopfzeile ---------- */

static void header_long(lv_event_t *e);

static void header_create(lv_obj_t *scr, int h)
{
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), h);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x0e1013), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, ui_px(20), 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hdr, header_long, LV_EVENT_LONG_PRESSED, NULL);

    s_clock = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_clock, ui_font(UI_F_L), 0);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(s_clock, "--:--");

    s_room = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_room, ui_font(UI_F_M), 0);
    lv_obj_set_style_text_color(s_room, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align(s_room, LV_ALIGN_LEFT_MID, ui_px(110), 0);
    lv_label_set_text(s_room, "");

    s_title = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_title, ui_font(UI_F_M), 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_title, "");

    s_lox_icon = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_lox_icon, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_lox_icon, LV_SYMBOL_HOME);
    lv_obj_align(s_lox_icon, LV_ALIGN_RIGHT_MID, 0, 0);

    s_wifi_icon = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_align_to(s_wifi_icon, s_lox_icon, LV_ALIGN_OUT_LEFT_MID, -ui_px(18), 0);
}

static void set_page_title(const char *title)
{
    lv_label_set_text(s_title, title ? title : "");
}

/* ---------- Einrichtung ---------- */

static lv_obj_t *s_setup_overlay;

static void setup_confirm_cb(lv_event_t *e)
{
    settings_request_setup();
    esp_restart();
}

static void msgbox_close_cb(lv_event_t *e)
{
    lv_msgbox_close(lv_event_get_user_data(e));
}

/* Lang auf die Kopfzeile drücken: Einrichtungsmodus (Access Point) starten */
static void header_long(lv_event_t *e)
{
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, "Einrichtung");
    lv_msgbox_add_text(mb, "Panel neu starten und den Einrichtungs-Access-Point öffnen? "
                           "WLAN und Miniserver lassen sich dann im Browser neu eintragen.");
    lv_obj_t *b1 = lv_msgbox_add_footer_button(mb, "Einrichtung starten");
    lv_obj_add_event_cb(b1, setup_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b2 = lv_msgbox_add_footer_button(mb, "Abbrechen");
    lv_obj_add_event_cb(b2, msgbox_close_cb, LV_EVENT_CLICKED, mb);
    lv_obj_set_width(mb, LV_PCT(60));
    lv_obj_set_style_text_font(mb, ui_font(UI_F_S), 0);
}

static void setup_overlay_show(void)
{
    if (s_setup_overlay) return;
    s_setup_overlay = lv_obj_create(lv_layer_top());
    lv_obj_t *o = s_setup_overlay;
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(o, ui_px(22), 0);

    lv_obj_t *t = lv_label_create(o);
    lv_obj_set_style_text_font(t, ui_font(UI_F_XL), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text(t, "Einrichtung");

    lv_obj_t *s1 = lv_label_create(o);
    lv_obj_set_style_text_font(s1, ui_font(UI_F_L), 0);
    lv_obj_set_style_text_color(s1, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text_fmt(s1, "1.  Mit dem WLAN  \"%s\"  verbinden", net_wifi_ap_ssid());

    lv_obj_t *s2 = lv_label_create(o);
    lv_obj_set_style_text_font(s2, ui_font(UI_F_L), 0);
    lv_obj_set_style_text_color(s2, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text(s2, "2.  Die Einrichtungsseite öffnet sich von selbst\n     - sonst im Browser  http://192.168.4.1  aufrufen");

    lv_obj_t *h = lv_label_create(o);
    lv_obj_set_style_text_font(h, ui_font(UI_F_M), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(UI_COL_DIM), 0);
    lv_label_set_text(h, "WLAN, Miniserver und Passwort eintragen - danach startet das Panel neu.");
}

static void header_update(void)
{
    if (net_time_valid()) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        lv_label_set_text_fmt(s_clock, "%02d:%02d", t.tm_hour, t.tm_min);
    }
    lv_label_set_text(s_room, cfg_get()->raum);

    net_wifi_status_t st;
    net_wifi_get_status(&st);
    if (st.state == NET_WIFI_SETUP_AP) setup_overlay_show();

    uint32_t wc = UI_COL_ERR;
    if (net_wifi_is_connected()) {
        net_wifi_status_t ws;
        net_wifi_get_status(&ws);
        wc = ws.rssi > -75 ? UI_COL_ON : UI_COL_WARN;
    }
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(wc), 0);

    lox_status_t ls;
    lox_get_status(&ls);
    uint32_t lc = ls.state == LOX_ONLINE ? UI_COL_ON
                : (ls.state == LOX_AUTH_FAILED || ls.state == LOX_IDLE) ? UI_COL_ERR : UI_COL_WARN;
    lv_obj_set_style_text_color(s_lox_icon, lv_color_hex(lc), 0);
}

static void ui_timer(lv_timer_t *t)
{
    static uint32_t n;
    cfg_poll();                 // Import/Undo aus der Weboberfläche übernehmen
    ui_pages_update();
    if (n++ % 5 == 0) {
        header_update();
        ui_diag_update();
    }
}

void ui_start(void)
{
    fonts_init();

    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(UI_COL_ON), lv_color_hex(0x3a7bd5), true,
                                           ui_font(UI_F_M));
    lv_display_set_theme(disp, th);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_text_font(scr, ui_font(UI_F_M), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const int hdr_h = ui_px(56);
    header_create(scr, hdr_h);
    ui_pages_init(scr, hdr_h, lv_display_get_vertical_resolution(disp) - hdr_h, set_page_title);

    header_update();
    lv_timer_create(ui_timer, 100, NULL);
}

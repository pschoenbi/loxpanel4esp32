/*
 * Diagnoseseite: WLAN, Loxone, Speicher, Firmware, Helligkeit.
 */

#include "ui_priv.h"

#include <stdio.h>
#include "board.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "lox_client.h"
#include "lox_struct.h"
#include "net_wifi.h"

static lv_obj_t *s_wifi;
static lv_obj_t *s_lox;
static lv_obj_t *s_sys;
static lv_obj_t *s_bright_lbl;

static lv_obj_t *diag_tile(lv_obj_t *parent, const char *title, lv_obj_t **body)
{
    lv_obj_t *t = ui_tile_create(parent);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, ui_px(6), 0);
    lv_obj_t *h = lv_label_create(t);
    lv_obj_set_style_text_font(h, ui_font(UI_F_S), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(UI_COL_DIM), 0);
    lv_label_set_text(h, title);
    *body = lv_label_create(t);
    lv_obj_set_width(*body, LV_PCT(100));
    lv_obj_set_style_text_font(*body, ui_font(UI_F_S), 0);
    lv_obj_set_style_text_line_space(*body, ui_px(4), 0);
    lv_label_set_text(*body, "-");
    return t;
}

static void bright_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    board_backlight_set((uint8_t)v);
    lv_label_set_text_fmt(s_bright_lbl, "Helligkeit %d %%", v);
}

void ui_diag_create(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(page, ui_px(16), 0);
    lv_obj_set_style_pad_row(page, ui_px(16), 0);

    lv_obj_t *t1 = diag_tile(page, "WLAN", &s_wifi);
    lv_obj_set_size(t1, LV_PCT(48), LV_PCT(55));
    lv_obj_t *t2 = diag_tile(page, "Loxone", &s_lox);
    lv_obj_set_size(t2, LV_PCT(48), LV_PCT(55));

    lv_obj_t *t3 = diag_tile(page, "System", &s_sys);
    lv_obj_set_size(t3, LV_PCT(48), LV_PCT(38));

    lv_obj_t *t4 = ui_tile_create(page);
    lv_obj_set_size(t4, LV_PCT(48), LV_PCT(38));
    lv_obj_set_flex_flow(t4, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t4, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    s_bright_lbl = lv_label_create(t4);
    lv_obj_set_style_text_font(s_bright_lbl, ui_font(UI_F_S), 0);
    lv_label_set_text_fmt(s_bright_lbl, "Helligkeit %d %%", board_backlight_get());
    lv_obj_t *sl = lv_slider_create(t4);
    lv_obj_set_size(sl, LV_PCT(95), ui_px(30));
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, board_backlight_get() ? board_backlight_get() : 80, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void ui_diag_update(void)
{
    net_wifi_status_t w;
    net_wifi_get_status(&w);
    lv_label_set_text_fmt(s_wifi,
                          "Status: %s\nSSID: %s\nIP: %s   (%s.local)\nSignal: %d dBm, Kanal %u\n"
                          "AP: %02x:%02x:%02x:%02x:%02x:%02x\nTrennungen %u, Roams %u\nC6-Firmware %s",
                          net_wifi_state_text(w.state), w.ssid[0] ? w.ssid : "-", w.ip[0] ? w.ip : "-",
                          w.hostname, w.rssi, w.channel,
                          w.bssid[0], w.bssid[1], w.bssid[2], w.bssid[3], w.bssid[4], w.bssid[5],
                          (unsigned)w.disconnects, (unsigned)w.roams, w.c6_fw[0] ? w.c6_fw : "?");

    lox_status_t l;
    lox_get_status(&l);
    lox_struct_info_t si = { 0 };
    if (lox_struct_loaded()) lox_struct_get_info(&si);
    lv_label_set_text_fmt(s_lox,
                          "Status: %s%s\nMiniserver: %s\nFirmware: %s  (%s, %s)\n"
                          "Struktur: %u Räume, %u Bausteine\nWerte %u, Texte %u, Anmeldungen %u\n%s",
                          lox_state_text(l.state), l.retry_in_s ? " (neuer Versuch läuft)" : "",
                          l.ms_name[0] ? l.ms_name : "-", l.fw_version[0] ? l.fw_version : "?",
                          l.hash_alg[0] ? l.hash_alg : "-", l.token_auth ? "Token" : "Passwort",
                          (unsigned)si.rooms, (unsigned)si.controls,
                          (unsigned)l.value_events, (unsigned)l.text_events, (unsigned)l.sessions,
                          l.last_error);

    const esp_app_desc_t *app = esp_app_get_description();
    const board_info_t *b = board_info();
    lv_label_set_text_fmt(s_sys, "Board %s (%ux%u)\nFirmware %s, IDF %s\nFrei: intern %u kB, PSRAM %u kB",
                          b->name, b->width, b->height, app->version, app->idf_ver,
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

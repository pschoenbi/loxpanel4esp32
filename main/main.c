/*
 * LoxPanel P4 – Start
 * Reihenfolge: NVS -> Board (Display, Touch) -> LVGL -> Oberfläche -> WLAN -> Uhrzeit -> Loxone
 */

#include "board.h"
#include "dev_settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lox_client.h"
#include "net_time.h"
#include "net_wifi.h"
#include "nvs_flash.h"
#include "panel_config.h"
#include "sdkconfig.h"
#include "ui.h"
#include "web.h"
#if CONFIG_LOXPANEL_C6_UPDATE
#include "net_c6_update.h"
#endif

static const char *TAG = "main";

#if CONFIG_LOXPANEL_C6_UPDATE
static lv_obj_t *s_upd_label;
static lv_obj_t *s_upd_bar;

static void c6_progress(const char *text, int percent)
{
    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_upd_label, text);
        if (percent >= 0) {
            lv_bar_set_value(s_upd_bar, percent, LV_ANIM_OFF);
        }
        lvgl_port_unlock();
    }
}

/* Läuft vor der Oberfläche. Kehrt nur zurück, wenn kein Neustart nötig ist. */
static void run_c6_update(void)
{
    if (lvgl_port_lock(0)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x1b1f24), 0);
        lv_obj_t *t = lv_label_create(scr);
        lv_label_set_text(t, "ESP32-C6 Firmware-Update");
        lv_obj_set_style_text_font(t, &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(t, lv_color_white(), 0);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, -120);
        s_upd_label = lv_label_create(scr);
        lv_obj_set_style_text_font(s_upd_label, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s_upd_label, lv_color_hex(0xffa657), 0);
        lv_obj_align(s_upd_label, LV_ALIGN_CENTER, 0, -30);
        lv_label_set_text(s_upd_label, "Bitte nicht ausschalten");
        s_upd_bar = lv_bar_create(scr);
        lv_obj_set_size(s_upd_bar, 700, 30);
        lv_obj_align(s_upd_bar, LV_ALIGN_CENTER, 0, 50);
        lv_bar_set_range(s_upd_bar, 0, 100);
        lvgl_port_unlock();
    }
    board_backlight_set(80);

    net_c6_update_result_t r = net_c6_update_run(c6_progress);
    if (r == NET_C6_UPDATE_DONE) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    vTaskDelay(pdMS_TO_TICKS(r == NET_C6_UPDATE_FAILED ? 8000 : 2000));
    if (lvgl_port_lock(0)) {
        lv_obj_clean(lv_screen_active());
        lvgl_port_unlock();
    }
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "LoxPanel P4 startet");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    settings_init();
    if (cfg_init() != ESP_OK) {
        ESP_LOGE(TAG, "Konfiguration nicht geladen");
    }

    ESP_ERROR_CHECK(board_init());

    if (board_lvgl_start() == NULL) {
        ESP_LOGE(TAG, "LVGL-Start fehlgeschlagen");
        return;
    }

#if CONFIG_LOXPANEL_C6_UPDATE
    run_c6_update();
#endif

    if (lvgl_port_lock(0)) {
        ui_start();
        lvgl_port_unlock();
    }

    /* Hintergrundbeleuchtung erst einschalten, wenn das erste Bild steht */
    vTaskDelay(pdMS_TO_TICKS(150));
    int bright = cfg_get()->helligkeit;
    board_backlight_set(bright >= 5 && bright <= 100 ? bright : 80);

    /* Einrichtungsmodus: kein WLAN eingerichtet oder am Display angefordert */
    static dev_settings_t s;
    settings_get(&s);
    const bool setup = !settings_has_wifi() || settings_take_setup_request();

    /* WLAN nach dem Display: Bild steht sofort, auch wenn der C6 hängt */
    const net_wifi_cfg_t wc = {
        .ssid = s.wifi_ssid, .password = s.wifi_pass,
        .ip = s.ip, .mask = s.mask, .gw = s.gw, .mac = s.mac,
        .setup_ap = setup,
    };
    if (net_wifi_start(&wc) != ESP_OK) {
        ESP_LOGE(TAG, "WLAN-Start fehlgeschlagen");
    }
    web_start(setup);

    if (!setup) {
        net_time_start();
        /* Loxone: versucht selbst so lange, bis WLAN und Miniserver erreichbar sind.
           Nach erhaltenem Token wird das Passwort gelöscht (SPEC 4.2). */
        lox_set_on_token(settings_forget_ms_password);
        if (lox_start(s.ms_host, s.ms_user, s.ms_pass) != ESP_OK) {
            ESP_LOGE(TAG, "Loxone-Start fehlgeschlagen");
        }
    }

    ESP_LOGI(TAG, "Gestartet. Frei: intern %u kB, PSRAM %u kB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

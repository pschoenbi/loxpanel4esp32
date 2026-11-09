#pragma once

/*
 * Hardware-Profil-Schnittstelle.
 * Jedes Board liefert genau diese Funktionen; der Rest der Firmware kennt keine Pins.
 */

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;   // Profilname, z. B. "crowpanel_p4_9in"
    uint16_t width;     // Auflösung in Pixel (Querformat)
    uint16_t height;
} board_info_t;

/* Profil-Informationen (gültig auch vor board_init). */
const board_info_t *board_info(void);

/* Stromversorgung, Display, Touch und Hintergrundbeleuchtung (aus) initialisieren. */
esp_err_t board_init(void);

esp_lcd_panel_handle_t board_panel(void);
esp_lcd_panel_io_handle_t board_panel_io(void);

/* NULL, falls kein Touch erkannt wurde. */
esp_lcd_touch_handle_t board_touch(void);

/* Helligkeit 0–100 %. 0 = aus. */
esp_err_t board_backlight_set(uint8_t percent);
uint8_t board_backlight_get(void);

/* LVGL starten und Display + Touch anmelden. Aufruf nach board_init(). */
lv_display_t *board_lvgl_start(void);

#ifdef __cplusplus
}
#endif

#pragma once

/*
 * Bedienoberfläche: feste Kopfzeile (Uhrzeit, Titel, Verbindungsstatus),
 * darunter wischbare Seiten mit Punkten. Auflösungsunabhängig (Basis 600 px Höhe).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Aufruf mit gehaltenem LVGL-Lock, nach board_lvgl_start(). */
void ui_start(void);

#ifdef __cplusplus
}
#endif

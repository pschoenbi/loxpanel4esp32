#pragma once

/*
 * Kamerabild (MJPEG-Stream oder einzelne JPEG-Bilder per HTTP):
 * Empfang -> Hardware-JPEG-Decoder -> Hardware-Skalierung (PPA) -> RGB565-Bild für LVGL.
 * Es läuft immer höchstens ein Stream.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const uint8_t *pixels;      // RGB565, Zeilenlänge = w * 2
    int w, h;
    uint32_t seq;               // Bildnummer
} cam_frame_t;

/* Stream starten (ein laufender wird vorher beendet). Bild wird auf max_w x max_h eingepasst. */
esp_err_t cam_start(const char *url, const char *user, const char *pass, int max_w, int max_h);

/* Stream beenden (kehrt sofort zurück, der Empfang endet kurz danach). */
void cam_stop(void);

/* Neuestes fertiges Bild übernehmen. Der Puffer bleibt gültig bis zum nächsten erfolgreichen Aufruf. */
bool cam_get_frame(cam_frame_t *out);

/* Kurzer Zustandstext für die Anzeige ("Verbinde ...", "Fehler 401", ...). */
void cam_status(char *out, size_t size);

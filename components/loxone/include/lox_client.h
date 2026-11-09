#pragma once

/*
 * Verbindung zum Loxone Miniserver (Gen 1, Gen 2, Compact):
 * WebSocket "remotecontrol", Token-Authentifizierung, Strukturdatei, binäre Status-Updates.
 * Hält die Verbindung selbstständig (Keepalive, Reconnect mit Backoff).
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lox_struct.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOX_IDLE,           // nicht konfiguriert
    LOX_CONNECTING,     // WebSocket wird aufgebaut
    LOX_AUTH,           // Schlüsselaustausch / Anmeldung
    LOX_LOADING,        // Strukturdatei wird geladen
    LOX_ONLINE,         // angemeldet, Status-Updates laufen
    LOX_OFFLINE,        // getrennt, nächster Versuch geplant
    LOX_AUTH_FAILED,    // Benutzer/Passwort falsch (kein automatischer Neuversuch in kurzer Folge)
} lox_state_t;

typedef struct {
    lox_state_t state;
    char fw_version[24];        // Miniserver-Firmware
    char ms_name[48];
    char last_error[64];
    char hash_alg[8];           // "SHA1" oder "SHA256"
    bool token_auth;            // mit gespeichertem Token angemeldet
    uint32_t value_events;      // empfangene Wert-Status
    uint32_t text_events;
    uint32_t sessions;          // erfolgreiche Anmeldungen seit Start
    uint32_t retry_in_s;
} lox_status_t;

/* Wird aus dem WebSocket-Task aufgerufen: kurz halten. */
typedef void (*lox_value_cb_t)(const lox_uuid_t *uuid, double value, void *ctx);
typedef void (*lox_text_cb_t)(const lox_uuid_t *uuid, const char *text, void *ctx);

esp_err_t lox_start(const char *host, const char *user, const char *password);
void lox_set_callbacks(lox_value_cb_t on_value, lox_text_cb_t on_text, void *ctx);
void lox_get_status(lox_status_t *out);
const char *lox_state_text(lox_state_t st);

/* Befehl an einen Baustein: jdev/sps/io/<uuid>/<cmd> (nur im Zustand ONLINE). */
esp_err_t lox_send_command(const char *uuid, const char *cmd);

/* Abfrage an einen Baustein (jdev/sps/io/<uuid>/<cmd>), Antwort asynchron.
 * cb läuft im WebSocket-Task (kurz halten, kein LVGL): code = HTTP-Code der Antwort
 * (0 = Zeitüberschreitung), value = Inhalt von LL.value als Text (JSON-Objekt unformatiert
 * bzw. Zeichenkette), sonst NULL. ctx wird unverändert übergeben. */
typedef void (*lox_query_cb_t)(void *ctx, int code, const char *value);
esp_err_t lox_query(const char *uuid, const char *cmd, lox_query_cb_t cb, void *ctx);

/* Binäre Abfrage (z. B. jdev/sps/getStatistic/...), Antwort asynchron im WebSocket-Task.
 * Es läuft immer nur eine; eine weitere während eine offen ist -> ESP_ERR_INVALID_STATE.
 * cb: code 200 + data/len bei Erfolg, sonst code (0 = Zeitüberschreitung) und data NULL. */
typedef void (*lox_bin_cb_t)(void *ctx, int code, const uint8_t *data, size_t len);
esp_err_t lox_query_bin(const char *path, lox_bin_cb_t cb, void *ctx);

/* Gesicherte Details eines Bausteins (z. B. Video-URL und Zugangsdaten einer Türsprechstelle),
 * beim Verbinden verschlüsselt abgefragt. Liefert eine Kopie des JSON-Texts (mit free freigeben)
 * oder NULL. Inhalt enthält Zugangsdaten: nie loggen. */
char *lox_secured_details(const char *uuid_action);

/* Gespeichertes Token löschen (z. B. nach Benutzerwechsel). */
void lox_forget_token(void);

/* Wird aufgerufen, sobald ein Token gespeichert wurde (Passwort kann dann weg). */
void lox_set_on_token(void (*cb)(void));

/* Liegt ein Token für host/user vor? */
bool lox_has_token(void);

#ifdef __cplusplus
}
#endif

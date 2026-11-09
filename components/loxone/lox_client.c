/*
 * Loxone-Miniserver-Client.
 *
 * Protokoll (Loxone-Doku "Communicating with the Miniserver"):
 *   - WebSocket ws://<host>/ws/rfc6455, Unterprotokoll "remotecontrol"
 *   - Jede Nachricht des Miniservers: 8-Byte-Binärkopf (0x03, Typ, Info, 0, Länge LE),
 *     danach der Inhalt als eigener Frame (Text, Datei, Wert-Status, Text-Status ...)
 *   - Anmeldung: RSA-Schlüssel holen -> AES-Session-Key per RSA tauschen -> getkey2
 *     -> HMAC über Benutzer/Passwort-Hash -> getjwt (verschlüsselt) -> Token speichern.
 *     Später nur noch authwithtoken + refreshjwt; das Passwort wird nicht mehr gebraucht.
 *   - Hash-Algorithmus (SHA1/SHA256) kommt aus der Antwort von getkey2,
 *     JWT vs. altes Token aus der Firmware-Version (ab 10.2 JWT).
 *
 * Aufbau: eine Task führt die Sitzung (Anfrage/Antwort nacheinander), der WebSocket-Task
 * setzt Nachrichten zusammen, verteilt Status-Updates direkt und legt Antworten in eine Queue.
 */

#include "lox_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lox_crypto.h"
#include "nvs.h"

static const char *TAG = "lox";

#define REQ_TIMEOUT_MS          10000
#define STRUCT_TIMEOUT_MS       60000
#define KEEPALIVE_INTERVAL_MS   30000
#define KEEPALIVE_DEAD_MS       90000
#define REFRESH_INTERVAL_US     (24LL * 3600 * 1000000)
#define BACKOFF_MIN_S           2
#define BACKOFF_MAX_S           60
#define AUTH_FAIL_WAIT_S        900     // falsche Anmeldedaten: selten versuchen, sonst sperrt der Miniserver
#define TOKEN_PERMISSION        4       // 4 = App (lange gültig), 2 = Web
#define NVS_NS                  "loxone"
#ifdef CONFIG_LOXPANEL_LOX_LOG_VALUES
#define LOG_VALUES              1
#else
#define LOG_VALUES              0
#endif
#define LOX_KEY_MAX             128     // getkey2-Schluessel (Bytes nach Hex-Dekodierung)

#define BIT_CONNECTED           BIT0
#define BIT_DISCONNECTED        BIT1

/* Typen im Nachrichtenkopf */
enum {
    MSG_TEXT = 0, MSG_FILE = 1, MSG_VALUES = 2, MSG_TEXTS = 3, MSG_DAYTIMER = 4,
    MSG_OUT_OF_SERVICE = 5, MSG_KEEPALIVE = 6, MSG_WEATHER = 7,
};

typedef struct {
    char *data;     // PSRAM, nullterminiert
    size_t len;
    uint8_t type;
} resp_t;

/* Konfiguration */
static char s_host[64], s_user[64], s_pass[96];
static char s_client_uuid[40];

/* Laufzeit */
static esp_websocket_client_handle_t s_ws;
static EventGroupHandle_t s_events;
static QueueHandle_t s_resp_q;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lox_status_t s_status;
static lox_value_cb_t s_on_value;
static lox_text_cb_t s_on_text;
static void *s_cb_ctx;

/* Empfang (nur WebSocket-Task) */
static char *s_rx;
static size_t s_rx_len, s_rx_cap;
static bool s_expect_payload;
static uint8_t s_payload_type;
static volatile int64_t s_last_keepalive_us;
static volatile bool s_out_of_service;
static int64_t s_log_from_us;           // Status-Änderungen erst nach dem ersten Komplettabzug loggen
static uint32_t s_log_budget;
static int64_t s_log_budget_reset_us;

/* Offene asynchrone Abfragen (lox_query) */
#define MAX_QUERIES     8
#define QUERY_TIMEOUT_US (15LL * 1000000)
typedef struct {
    char key[200];          // "sps/io/<uuid>/<cmd>"
    lox_query_cb_t cb;
    void *ctx;
    int64_t deadline;
} query_t;
static query_t s_queries[MAX_QUERIES];

/* Offene binäre Abfrage (lox_query_bin) */
static lox_bin_cb_t s_bin_cb;
static void *s_bin_ctx;
static int64_t s_bin_deadline;
static char s_bin_key[48];      // z. B. "getStatistic" zum Erkennen von Fehlerantworten

/* Gesicherte Details (Türsprechstellen) */
#define MAX_SECURED 6
typedef struct {
    char uuid[40];
    char *json;             // PSRAM
} secured_t;
static secured_t s_secured[MAX_SECURED];
static SemaphoreHandle_t s_secured_mx;
static int s_secured_count;

/* Sitzung */
static uint8_t s_aes_key[32], s_aes_iv[16];
static char s_salt[8];
static bool s_use_jwt = true;
static bool s_sha256;

/* ------------------------------------------------------------------ */

const char *lox_state_text(lox_state_t st)
{
    switch (st) {
    case LOX_IDLE:        return "nicht eingerichtet";
    case LOX_CONNECTING:  return "verbindet";
    case LOX_AUTH:        return "meldet an";
    case LOX_LOADING:     return "laedt Struktur";
    case LOX_ONLINE:      return "online";
    case LOX_OFFLINE:     return "getrennt";
    case LOX_AUTH_FAILED: return "Anmeldung abgelehnt";
    }
    return "?";
}

void lox_get_status(lox_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

static void set_state(lox_state_t st)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = st;
    portEXIT_CRITICAL(&s_lock);
}

static void set_error(const char *fmt, const char *arg)
{
    char msg[64];
    snprintf(msg, sizeof(msg), fmt, arg ? arg : "");
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.last_error, msg, sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "%s", msg);
}

void lox_set_callbacks(lox_value_cb_t on_value, lox_text_cb_t on_text, void *ctx)
{
    s_on_value = on_value;
    s_on_text = on_text;
    s_cb_ctx = ctx;
}

/* ------------------------------------------------------------------ */
/* Token im NVS                                                        */

static bool token_load(char *token, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char host[64] = "", user[64] = "";
    size_t l1 = sizeof(host), l2 = sizeof(user), l3 = size;
    bool ok = nvs_get_str(h, "host", host, &l1) == ESP_OK &&
              nvs_get_str(h, "user", user, &l2) == ESP_OK &&
              nvs_get_str(h, "token", token, &l3) == ESP_OK &&
              strcmp(host, s_host) == 0 && strcmp(user, s_user) == 0 && token[0];
    nvs_close(h);
    return ok;
}

static void token_save(const char *token)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "host", s_host);
    nvs_set_str(h, "user", s_user);
    nvs_set_str(h, "token", token);
    nvs_commit(h);
    nvs_close(h);
}

static void (*s_on_token)(void);

void lox_set_on_token(void (*cb)(void))
{
    s_on_token = cb;
}

bool lox_has_token(void)
{
    char *t = malloc(1024);
    bool ok = t && token_load(t, 1024);
    free(t);
    return ok;
}

void lox_forget_token(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "token");
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* Empfang                                                             */

static void handle_values(const uint8_t *p, size_t len)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_log_budget_reset_us > 1000000) {
        s_log_budget_reset_us = now;
        s_log_budget = 20;          // höchstens 20 Log-Zeilen pro Sekunde
    }
    for (size_t off = 0; off + 24 <= len; off += 24) {
        lox_uuid_t u;
        lox_uuid_from_wire(p + off, &u);
        double v;
        memcpy(&v, p + off + 16, sizeof(v));
        lox_value_set(&u, v);
        if (s_on_value) s_on_value(&u, v, s_cb_ctx);

        if (LOG_VALUES && now >= s_log_from_us && s_log_budget > 0) {
            s_log_budget--;
            const lox_state_ref_t *r = lox_struct_find_state(&u);
            if (r) {
                ESP_LOGI(TAG, "Wert  %s / %s [%s]%s%s = %g", r->control_name, r->state_name, r->control_type,
                         r->room_name ? " in " : "", r->room_name ? r->room_name : "", v);
            } else {
                char us[37];
                lox_uuid_format(&u, us);
                ESP_LOGI(TAG, "Wert  %s = %g", us, v);
            }
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_status.value_events += len / 24;
    portEXIT_CRITICAL(&s_lock);
}

static void handle_texts(const uint8_t *p, size_t len)
{
    const int64_t now = esp_timer_get_time();
    uint32_t count = 0;
    size_t off = 0;
    while (off + 36 <= len) {
        lox_uuid_t u;
        lox_uuid_from_wire(p + off, &u);
        uint32_t tl;
        memcpy(&tl, p + off + 32, 4);
        if (off + 36 + tl > len) break;
        char *txt = malloc(tl + 1);
        if (txt) {
            memcpy(txt, p + off + 36, tl);
            txt[tl] = '\0';
            lox_text_set(&u, txt);
            if (s_on_text) s_on_text(&u, txt, s_cb_ctx);
            if (LOG_VALUES && now >= s_log_from_us && s_log_budget > 0) {
                s_log_budget--;
                const lox_state_ref_t *r = lox_struct_find_state(&u);
                ESP_LOGI(TAG, "Text  %s / %s = \"%s\"", r ? r->control_name : "?", r ? r->state_name : "?", txt);
            }
            free(txt);
        }
        off += 36 + ((tl + 3) & ~3u);   // auf 4 Bytes aufgefüllt
        count++;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.text_events += count;
    portEXIT_CRITICAL(&s_lock);
}

/* Daytimer-Status: uuid(16), Standardwert(double), Anzahl(int32), je Eintrag
   mode, from, to, needActivate (int32) + value (double) = 24 Bytes */
static void handle_daytimers(const uint8_t *p, size_t len)
{
    size_t off = 0;
    while (off + 28 <= len) {
        lox_uuid_t u;
        lox_uuid_from_wire(p + off, &u);
        double def;
        int32_t n;
        memcpy(&def, p + off + 16, 8);
        memcpy(&n, p + off + 24, 4);
        off += 28;
        if (n < 0 || off + (size_t)n * 24 > len) break;
        lox_dt_entry_t *e = n ? malloc(n * sizeof(*e)) : NULL;
        if (n && e == NULL) break;
        for (int i = 0; i < n; i++) {
            const uint8_t *q = p + off + i * 24;
            int32_t v[4];
            memcpy(v, q, 16);
            e[i].mode = v[0];
            e[i].from_min = v[1];
            e[i].to_min = v[2];
            e[i].need_activate = v[3];
            memcpy(&e[i].value, q + 16, 8);
        }
        lox_daytimer_set(&u, def, e, n);
        free(e);
        off += (size_t)n * 24;
    }
}

static esp_err_t send_text(const char *cmd);

/* Antwort auf eine lox_query-Abfrage? Dann Rückruf aufrufen und true liefern. */
static bool handle_query_response(const char *text, size_t len)
{
    bool any = false;
    for (int i = 0; i < MAX_QUERIES && !any; i++) any = s_queries[i].cb != NULL;
    if (!any) return false;

    cJSON *root = cJSON_ParseWithLength(text, len);
    const cJSON *ll = cJSON_GetObjectItem(root, "LL");
    const cJSON *ctl = cJSON_GetObjectItem(ll, "control");
    if (!cJSON_IsString(ctl)) {
        cJSON_Delete(root);
        return false;
    }
    /* passende Abfrage suchen (Miniserver wiederholt den Befehl, ggf. ohne "j" am Anfang) */
    query_t q = { 0 };
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MAX_QUERIES; i++) {
        if (s_queries[i].cb && strstr(ctl->valuestring, s_queries[i].key)) {
            q = s_queries[i];
            s_queries[i].cb = NULL;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (q.cb == NULL) {
        ESP_LOGD(TAG, "Antwort ohne passende Abfrage: %.120s", ctl->valuestring);
        cJSON_Delete(root);
        return false;
    }
    const cJSON *c = cJSON_GetObjectItem(ll, "Code");
    if (c == NULL) c = cJSON_GetObjectItem(ll, "code");
    int code = cJSON_IsNumber(c) ? c->valueint : (cJSON_IsString(c) ? atoi(c->valuestring) : 0);
    const cJSON *v = cJSON_GetObjectItem(ll, "value");
    char *printed = NULL;
    const char *vs = NULL;
    if (cJSON_IsString(v)) vs = v->valuestring;
    else if (v) vs = printed = cJSON_PrintUnformatted(v);
    q.cb(q.ctx, code, vs);
    cJSON_free(printed);
    cJSON_Delete(root);
    return true;
}

/* Binäre Antwort oder Fehler zur offenen lox_query_bin? (im WebSocket-Task) */
static bool handle_bin_response(uint8_t type, const char *data, size_t len)
{
    lox_bin_cb_t cb = NULL;
    void *ctx = NULL;
    int code = 0;
    portENTER_CRITICAL(&s_lock);
    if (s_bin_cb) {
        if (type == MSG_FILE) {
            cb = s_bin_cb;
            code = 200;
        } else if (type == MSG_TEXT && s_bin_key[0] && strstr(data, s_bin_key)) {
            cb = s_bin_cb;
        }
        if (cb) {
            ctx = s_bin_ctx;
            s_bin_cb = NULL;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (cb == NULL) return false;
    if (code == 200) {
        cb(ctx, 200, (const uint8_t *)data, len);
    } else {
        const char *cs = strstr(data, "\"Code\": \"");
        if (cs == NULL) cs = strstr(data, "\"code\": \"");
        code = cs ? atoi(cs + 9) : 0;
        ESP_LOGW(TAG, "Binäre Abfrage abgelehnt: %.160s", data);
        cb(ctx, code ? code : 500, NULL, 0);
    }
    return true;
}

esp_err_t lox_query_bin(const char *path, lox_bin_cb_t cb, void *ctx)
{
    if (s_ws == NULL || s_status.state != LOX_ONLINE) return ESP_ERR_INVALID_STATE;
    int64_t now = esp_timer_get_time();
    lox_bin_cb_t old = NULL;
    void *old_ctx = NULL;
    bool busy = false;
    portENTER_CRITICAL(&s_lock);
    if (s_bin_cb && s_bin_deadline < now) {
        old = s_bin_cb;
        old_ctx = s_bin_ctx;
        s_bin_cb = NULL;
    }
    if (s_bin_cb) {
        busy = true;
    } else {
        s_bin_cb = cb;
        s_bin_ctx = ctx;
        s_bin_deadline = now + QUERY_TIMEOUT_US;
        /* Befehlsname (3. Pfadteil) zum Zuordnen einer Fehlerantwort */
        const char *p = strstr(path, "sps/");
        p = p ? p + 4 : path;
        size_t n = strcspn(p, "/");
        if (n >= sizeof(s_bin_key)) n = sizeof(s_bin_key) - 1;
        memcpy(s_bin_key, p, n);
        s_bin_key[n] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
    if (old) old(old_ctx, 0, NULL, 0);
    if (busy) return ESP_ERR_INVALID_STATE;
    ESP_LOGD(TAG, "Binäre Abfrage %s", path);
    esp_err_t err = send_text(path);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_bin_cb = NULL;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

esp_err_t lox_query(const char *uuid, const char *cmd, lox_query_cb_t cb, void *ctx)
{
    if (s_ws == NULL || s_status.state != LOX_ONLINE) return ESP_ERR_INVALID_STATE;
    int64_t now = esp_timer_get_time();
    query_t expired[MAX_QUERIES];
    int nexp = 0, slot = -1;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MAX_QUERIES; i++) {
        if (s_queries[i].cb && s_queries[i].deadline < now) {
            expired[nexp++] = s_queries[i];
            s_queries[i].cb = NULL;
        }
        if (s_queries[i].cb == NULL && slot < 0) slot = i;
    }
    if (slot >= 0) {
        snprintf(s_queries[slot].key, sizeof(s_queries[slot].key), "sps/io/%s/%s", uuid, cmd);
        s_queries[slot].cb = cb;
        s_queries[slot].ctx = ctx;
        s_queries[slot].deadline = now + QUERY_TIMEOUT_US;
    }
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < nexp; i++) expired[i].cb(expired[i].ctx, 0, NULL);
    if (slot < 0) return ESP_ERR_NO_MEM;

    char buf[700];
    snprintf(buf, sizeof(buf), "jdev/sps/io/%s/%s", uuid, cmd);
    ESP_LOGD(TAG, "Abfrage %s", buf);
    esp_err_t err = send_text(buf);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_queries[slot].cb = NULL;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

static void dispatch_message(uint8_t opcode)
{
    if (!s_expect_payload) {
        /* Erwartet wird ein 8-Byte-Kopf */
        if (opcode != 0x2 || s_rx_len != 8 || (uint8_t)s_rx[0] != 0x03) {
            ESP_LOGW(TAG, "Unerwartete Nachricht ohne Kopf (Op %u, %u Bytes): %.120s",
                     opcode, (unsigned)s_rx_len, opcode == 0x1 ? s_rx : "");
            return;
        }
        uint8_t type = (uint8_t)s_rx[1];
        uint8_t info = (uint8_t)s_rx[2];
        if (type == MSG_KEEPALIVE) {
            s_last_keepalive_us = esp_timer_get_time();
            return;
        }
        if (type == MSG_OUT_OF_SERVICE) {
            ESP_LOGW(TAG, "Miniserver meldet: ausser Betrieb (Neustart/Update)");
            s_out_of_service = true;
            xEventGroupSetBits(s_events, BIT_DISCONNECTED);
            return;
        }
        if (info & 0x80) {
            return;     // "Estimated" (höchstes Bit): geschätzte Länge, der genaue Kopf folgt
        }
        s_expect_payload = true;
        s_payload_type = type;
        return;
    }

    /* Ein weiterer Kopf statt Inhalt: Kopf neu auswerten. Echte Inhalte sind nie 8 Bytes mit 0x03
       (Wert-Status sind Vielfache von 24, Text-Status >= 36 Bytes, Texte beginnen mit '{'). */
    if (opcode == 0x2 && s_rx_len == 8 && (uint8_t)s_rx[0] == 0x03 && (uint8_t)s_rx[1] <= MSG_WEATHER) {
        s_expect_payload = false;
        dispatch_message(opcode);
        return;
    }

    s_expect_payload = false;
    s_last_keepalive_us = esp_timer_get_time();     // jede Nachricht zählt als Lebenszeichen

    switch (s_payload_type) {
    case MSG_VALUES:
        handle_values((const uint8_t *)s_rx, s_rx_len);
        break;
    case MSG_TEXTS:
        handle_texts((const uint8_t *)s_rx, s_rx_len);
        break;
    case MSG_TEXT:
    case MSG_FILE: {
        /* Antworten auf Bedienbefehle (jdev/sps/io/...) nur protokollieren, nicht in die Queue */
        if (s_status.state == LOX_ONLINE && handle_bin_response(s_payload_type, s_rx, s_rx_len)) break;
        if (s_status.state == LOX_ONLINE && s_payload_type == MSG_TEXT && strstr(s_rx, "sps/io/")) {
            if (handle_query_response(s_rx, s_rx_len)) break;
            const char *code = strstr(s_rx, "\"Code\": \"");
            if (code == NULL) code = strstr(s_rx, "\"code\": \"");
            int c = code ? atoi(code + 9) : 0;
            if (c == 200) {
                ESP_LOGI(TAG, "Befehl bestätigt");
            } else {
                ESP_LOGW(TAG, "Befehl abgelehnt: %.200s", s_rx);
            }
            break;
        }
        resp_t r = { .data = s_rx, .len = s_rx_len, .type = s_payload_type };
        if (xQueueSend(s_resp_q, &r, 0) == pdTRUE) {
            s_rx = NULL;        // gehört jetzt der Queue
            s_rx_cap = 0;
        }
        break;
    }
    case MSG_DAYTIMER:
        handle_daytimers((const uint8_t *)s_rx, s_rx_len);
        break;
    default:
        break;                  // Wetter: später
    }
}

static bool rx_reserve(size_t need)
{
    if (need <= s_rx_cap) return true;
    size_t cap = s_rx_cap ? s_rx_cap : 4096;
    while (cap < need) cap *= 2;
    char *n = heap_caps_realloc(s_rx, cap, MALLOC_CAP_SPIRAM);
    if (n == NULL) return false;
    s_rx = n;
    s_rx_cap = cap;
    return true;
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = data;
    static uint8_t cur_op;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
        break;
    case WEBSOCKET_EVENT_DATA: {
        uint8_t op = ev->op_code;
        if (op >= 0x8) break;                       // Close/Ping/Pong
        if (op != 0x0 && ev->payload_offset == 0) { // neue Nachricht
            cur_op = op;
            s_rx_len = 0;
        }
        if (!rx_reserve(s_rx_len + ev->data_len + 1)) {
            ESP_LOGE(TAG, "Kein Speicher für %d Bytes", (int)(s_rx_len + ev->data_len));
            s_rx_len = 0;
            break;
        }
        memcpy(s_rx + s_rx_len, ev->data_ptr, ev->data_len);
        s_rx_len += ev->data_len;
        s_rx[s_rx_len] = '\0';
        bool frame_done = ev->payload_offset + ev->data_len >= ev->payload_len;
        if (frame_done && ev->fin) {
            dispatch_message(cur_op);
            s_rx_len = 0;
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Anfrage / Antwort                                                   */

static void drain_responses(void)
{
    resp_t r;
    while (xQueueReceive(s_resp_q, &r, 0) == pdTRUE) {
        heap_caps_free(r.data);
    }
}

static esp_err_t send_text(const char *cmd)
{
    int n = esp_websocket_client_send_text(s_ws, cmd, strlen(cmd), pdMS_TO_TICKS(5000));
    return n >= 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t request_raw(const char *cmd, int timeout_ms, resp_t *out)
{
    drain_responses();
    if (send_text(cmd) != ESP_OK) return ESP_FAIL;
    if (xQueueReceive(s_resp_q, out, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* Antwort {"LL": {"control": ..., "value": ..., "Code": "200"}} */
typedef struct {
    cJSON *root;
    const cJSON *value;
    int code;
} ll_t;

static void ll_free(ll_t *ll)
{
    if (ll->root) cJSON_Delete(ll->root);
    ll->root = NULL;
}

/* Text -> ll (Text wird nicht freigegeben) */
static esp_err_t parse_ll(const char *cmd, const char *text, size_t len, ll_t *ll)
{
    ll->root = cJSON_ParseWithLength(text, len);
    const cJSON *l = ll->root ? cJSON_GetObjectItem(ll->root, "LL") : NULL;
    if (l == NULL) {
        ESP_LOGW(TAG, "Anfrage %.40s: unerwartete Antwort (%u Bytes): %.200s", cmd, (unsigned)len, text);
        ll_free(ll);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGD(TAG, "Antwort auf %.40s: %.300s", cmd, text);
    const cJSON *c = cJSON_GetObjectItem(l, "Code");
    if (c == NULL) c = cJSON_GetObjectItem(l, "code");
    ll->code = cJSON_IsNumber(c) ? c->valueint : (cJSON_IsString(c) ? atoi(c->valuestring) : 0);
    ll->value = cJSON_GetObjectItem(l, "value");
    return ESP_OK;
}

static esp_err_t request(const char *cmd, ll_t *ll, int timeout_ms)
{
    memset(ll, 0, sizeof(*ll));
    resp_t r;
    esp_err_t err = request_raw(cmd, timeout_ms, &r);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Anfrage %.40s: %s", cmd, esp_err_to_name(err));
        return err;
    }
    err = parse_ll(cmd, r.data, r.len, ll);
    heap_caps_free(r.data);
    return err;
}

/*
 * Unverschlüsselte Info-Abfragen per HTTP. Ab Firmware 17 beantwortet der Miniserver
 * jdev/cfg/apiKey und jdev/sys/getPublicKey über den WebSocket nur mit "Bad request".
 */
static esp_err_t http_request(const char *path, ll_t *ll)
{
    memset(ll, 0, sizeof(*ll));
    char url[160];
    snprintf(url, sizeof(url), "http://%s/%s", s_host, path);
    const esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;

    esp_err_t err = ESP_FAIL;
    const size_t cap = 8192;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (buf && esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        size_t len = 0;
        int n;
        while (len < cap - 1 && (n = esp_http_client_read(c, buf + len, cap - 1 - len)) > 0) {
            len += n;
        }
        buf[len] = '\0';
        if (status == 200) {
            err = parse_ll(path, buf, len, ll);
        } else {
            ESP_LOGW(TAG, "HTTP %s: Status %d", path, status);
        }
    } else {
        ESP_LOGW(TAG, "HTTP %s: keine Verbindung", path);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    heap_caps_free(buf);
    return err;
}

/* Verschlüsselter Befehl: jdev/sys/enc/<urlenc(base64(AES("salt/<salt>/<cmd>")))> */
static esp_err_t request_enc(const char *cmd, ll_t *ll)
{
    size_t plain_len = strlen(cmd) + 32;
    char *plain = malloc(plain_len);
    char *b64 = malloc(plain_len * 2 + 64);
    char *full = malloc(plain_len * 6 + 64);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (plain && b64 && full) {
        snprintf(plain, plain_len, "salt/%s/%s", s_salt, cmd);
        err = lox_aes_encrypt_b64(s_aes_key, s_aes_iv, plain, b64, plain_len * 2 + 64);
        if (err == ESP_OK) {
            strcpy(full, "jdev/sys/enc/");
            lox_url_encode(b64, full + strlen(full), plain_len * 6 + 64 - strlen(full));
            err = request(full, ll, REQ_TIMEOUT_MS);
        }
    }
    free(plain);
    free(b64);
    free(full);
    return err;
}

/* ------------------------------------------------------------------ */
/* Anmeldung                                                           */

static void read_fw_version(void)
{
    ll_t ll;
    s_use_jwt = true;
    if (http_request("jdev/cfg/apiKey", &ll) != ESP_OK) return;
    /* value: "{'snr': '50:4F:94:..', 'version':'14.5.12.7', ...}" */
    if (cJSON_IsString(ll.value)) {
        const char *v = strstr(ll.value->valuestring, "'version':'");
        if (v) {
            v += strlen("'version':'");
            char ver[24] = { 0 };
            for (int i = 0; i < 23 && v[i] && v[i] != '\''; i++) ver[i] = v[i];
            int major = 0, minor = 0;
            sscanf(ver, "%d.%d", &major, &minor);
            s_use_jwt = major > 10 || (major == 10 && minor >= 2);
            portENTER_CRITICAL(&s_lock);
            strlcpy(s_status.fw_version, ver, sizeof(s_status.fw_version));
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "Miniserver-Firmware %s (%s)", ver, s_use_jwt ? "JWT" : "Token alt");
        }
    }
    ll_free(&ll);
}

static esp_err_t key_exchange(void)
{
    ll_t ll;
    esp_err_t err = http_request("jdev/sys/getPublicKey", &ll);
    if (err != ESP_OK || ll.code != 200 || !cJSON_IsString(ll.value)) {
        ESP_LOGW(TAG, "getPublicKey: %s, Code %d", esp_err_to_name(err), ll.code);
        ll_free(&ll);
        set_error("Public Key nicht erhalten%s", NULL);
        return ESP_FAIL;
    }
    /* "-----BEGIN CERTIFICATE-----MIGf...-----END CERTIFICATE-----" -> nur Base64 */
    const char *pem = ll.value->valuestring;
    const char *b = strstr(pem, "-----BEGIN");
    b = b ? strstr(b + 10, "-----") : NULL;
    const char *e = b ? strstr(b + 5, "-----END") : NULL;
    if (b == NULL || e == NULL) {
        ll_free(&ll);
        set_error("Public Key unbekanntes Format%s", NULL);
        return ESP_FAIL;
    }
    b += 5;
    char *key_b64 = malloc(e - b + 1);
    size_t n = 0;
    for (const char *p = b; p < e; p++) {
        if (!isspace((unsigned char)*p)) key_b64[n++] = *p;
    }
    key_b64[n] = '\0';
    ll_free(&ll);

    /* Session-Key: "<hex key>:<hex iv>", per RSA verschlüsselt */
    lox_random(s_aes_key, sizeof(s_aes_key));
    lox_random(s_aes_iv, sizeof(s_aes_iv));
    uint8_t salt_raw[2];
    lox_random(salt_raw, 2);
    lox_hex_encode(salt_raw, 2, s_salt, false);

    char session[64 + 1 + 32 + 1];
    lox_hex_encode(s_aes_key, 32, session, false);
    session[64] = ':';
    lox_hex_encode(s_aes_iv, 16, session + 65, false);

    /* RSA bis 4096 Bit: 512 Bytes -> 684 Zeichen Base64 */
    char *enc_b64 = malloc(800);
    char *cmd = malloc(900);
    if (enc_b64 == NULL || cmd == NULL) {
        free(key_b64);
        free(enc_b64);
        free(cmd);
        return ESP_ERR_NO_MEM;
    }
    err = lox_rsa_encrypt_b64(key_b64, (const uint8_t *)session, strlen(session), enc_b64, 800);
    free(key_b64);
    if (err != ESP_OK) {
        free(enc_b64);
        free(cmd);
        set_error("RSA-Verschluesselung fehlgeschlagen%s", NULL);
        return err;
    }

    snprintf(cmd, 900, "jdev/sys/keyexchange/%s", enc_b64);
    free(enc_b64);
    err = request(cmd, &ll, REQ_TIMEOUT_MS);
    free(cmd);
    int code = ll.code;
    ll_free(&ll);
    if (err != ESP_OK || code != 200) {
        set_error("Schluesselaustausch abgelehnt%s", NULL);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* getkey2: Schlüssel (hex), Benutzer-Salt und Hash-Algorithmus */
static esp_err_t get_key2(uint8_t *key, size_t *key_len, char *user_salt, size_t salt_size)
{
    char user_enc[200], cmd[256];
    lox_url_encode(s_user, user_enc, sizeof(user_enc));
    snprintf(cmd, sizeof(cmd), "jdev/sys/getkey2/%s", user_enc);
    ll_t ll;
    esp_err_t err = request(cmd, &ll, REQ_TIMEOUT_MS);
    if (err != ESP_OK || ll.code != 200 || !cJSON_IsObject(ll.value)) {
        int code = ll.code;
        ll_free(&ll);
        ESP_LOGW(TAG, "getkey2: Code %d", code);
        return code == 401 || code == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    const cJSON *k = cJSON_GetObjectItem(ll.value, "key");
    const cJSON *s = cJSON_GetObjectItem(ll.value, "salt");
    const cJSON *a = cJSON_GetObjectItem(ll.value, "hashAlg");
    s_sha256 = cJSON_IsString(a) && strcmp(a->valuestring, "SHA256") == 0;
    /* Längen prüfen: abgeschnittener Salt/Schlüssel ergibt still einen falschen Hash (401) */
    bool fits = cJSON_IsString(k) && strlen(k->valuestring) / 2 <= LOX_KEY_MAX &&
                (!cJSON_IsString(s) || strlen(s->valuestring) < salt_size);
    int kl = fits ? lox_hex_decode(k->valuestring, key, LOX_KEY_MAX) : -1;
    if (fits && cJSON_IsString(s)) strlcpy(user_salt, s->valuestring, salt_size);
    ESP_LOGI(TAG, "getkey2: %s, Schluessel %d Bytes, Salt %u Zeichen", s_sha256 ? "SHA256" : "SHA1",
             kl, (unsigned)strlen(user_salt));
    ll_free(&ll);
    if (!fits) {
        set_error("getkey2: Schluessel/Salt zu lang%s", NULL);
        return ESP_FAIL;
    }
    if (kl <= 0) return ESP_FAIL;
    *key_len = kl;
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.hash_alg, s_sha256 ? "SHA256" : "SHA1", sizeof(s_status.hash_alg));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static void store_token_from(const cJSON *value)
{
    const cJSON *t = cJSON_GetObjectItem(value, "token");
    if (cJSON_IsString(t) && t->valuestring[0]) {
        token_save(t->valuestring);
        ESP_LOGI(TAG, "Token gespeichert (%u Zeichen)", (unsigned)strlen(t->valuestring));
        if (s_on_token) s_on_token();
    }
    const cJSON *u = cJSON_GetObjectItem(value, "unsecurePass");
    if (cJSON_IsTrue(u)) {
        ESP_LOGW(TAG, "Miniserver meldet: Passwort des Benutzers ist unsicher");
    }
}

/* Mit gespeichertem Token anmelden und danach erneuern */
static esp_err_t auth_with_token(const char *token)
{
    uint8_t key[LOX_KEY_MAX];
    size_t kl;
    char usalt[160];
    if (get_key2(key, &kl, usalt, sizeof(usalt)) != ESP_OK) return ESP_FAIL;

    char hash[65], cmd[320];
    lox_hmac_hex(s_sha256, key, kl, token, hash);
    char user_enc[200];
    lox_url_encode(s_user, user_enc, sizeof(user_enc));
    snprintf(cmd, sizeof(cmd), "authwithtoken/%s/%s", hash, user_enc);
    ll_t ll;
    esp_err_t err = request_enc(cmd, &ll);
    int code = ll.code;
    ll_free(&ll);
    if (err != ESP_OK || code != 200) {
        ESP_LOGW(TAG, "Token abgelehnt (Code %d)", code);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Mit Token angemeldet");
    return ESP_OK;
}

static void refresh_token(void)
{
    char token[1024];
    if (!token_load(token, sizeof(token))) return;
    uint8_t key[LOX_KEY_MAX];
    size_t kl;
    char usalt[160];
    if (get_key2(key, &kl, usalt, sizeof(usalt)) != ESP_OK) return;

    char hash[65], user_enc[200], cmd[320];
    lox_hmac_hex(s_sha256, key, kl, token, hash);
    lox_url_encode(s_user, user_enc, sizeof(user_enc));
    snprintf(cmd, sizeof(cmd), "jdev/sys/%s/%s/%s", s_use_jwt ? "refreshjwt" : "refreshtoken", hash, user_enc);
    ll_t ll;
    if (request_enc(cmd, &ll) == ESP_OK && ll.code == 200) {
        store_token_from(ll.value);     // ab 10.2 kommt ein neues JWT zurück
        ESP_LOGI(TAG, "Token erneuert");
    } else {
        ESP_LOGW(TAG, "Token-Erneuerung fehlgeschlagen (Code %d)", ll.code);
    }
    ll_free(&ll);
}

/* Mit Benutzer/Passwort ein Token holen. Liefert ESP_ERR_INVALID_ARG bei falschem Passwort. */
static esp_err_t auth_with_password(void)
{
    uint8_t key[LOX_KEY_MAX];
    size_t kl;
    char usalt[160] = "";
    esp_err_t err = get_key2(key, &kl, usalt, sizeof(usalt));
    if (err == ESP_ERR_NOT_FOUND) return ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) return err;

    /* pwHash = HASH(passwort:salt) in Grossbuchstaben, hash = HMAC(key, benutzer:pwHash) */
    char *tmp = malloc(strlen(s_pass) + strlen(usalt) + 2);
    sprintf(tmp, "%s:%s", s_pass, usalt);
    char pwhash[65];
    lox_hash_hex(s_sha256, tmp, pwhash, true);
    free(tmp);

    char userpw[200], hash[65];
    snprintf(userpw, sizeof(userpw), "%s:%s", s_user, pwhash);
    lox_hmac_hex(s_sha256, key, kl, userpw, hash);

    char user_enc[200], cmd[512];
    lox_url_encode(s_user, user_enc, sizeof(user_enc));
    snprintf(cmd, sizeof(cmd), "jdev/sys/%s/%s/%s/%d/%s/LoxPanel",
             s_use_jwt ? "getjwt" : "gettoken", hash, user_enc, TOKEN_PERMISSION, s_client_uuid);
    ll_t ll;
    err = request_enc(cmd, &ll);
    int code = ll.code;
    if (err == ESP_OK && code == 200 && cJSON_IsObject(ll.value)) {
        store_token_from(ll.value);
        ll_free(&ll);
        ESP_LOGI(TAG, "Mit Passwort angemeldet, Token erhalten");
        return ESP_OK;
    }
    ll_free(&ll);
    ESP_LOGW(TAG, "Anmeldung abgelehnt (Code %d)", code);
    return code == 401 ? ESP_ERR_INVALID_ARG : ESP_FAIL;
}

static esp_err_t authenticate(void)
{
    char *token = malloc(1024);
    if (token == NULL) return ESP_ERR_NO_MEM;
    bool have = token_load(token, 1024);
    esp_err_t err = ESP_FAIL;
    if (have) {
        err = auth_with_token(token);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_status.token_auth = true;
            portEXIT_CRITICAL(&s_lock);
            free(token);
            refresh_token();
            return ESP_OK;
        }
        if (err == ESP_ERR_INVALID_STATE) lox_forget_token();
    }
    free(token);
    if (s_pass[0] == '\0') {
        set_error("Kein gueltiges Token und kein Passwort%s", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    err = auth_with_password();
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.token_auth = false;
        portEXIT_CRITICAL(&s_lock);
    } else if (err == ESP_ERR_INVALID_ARG) {
        set_error("Benutzer oder Passwort falsch%s", NULL);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Struktur                                                            */

static esp_err_t load_structure(void)
{
    ll_t ll;
    char version[32] = "";
    if (request("jdev/sps/LoxAPPversion3", &ll, REQ_TIMEOUT_MS) == ESP_OK && cJSON_IsString(ll.value)) {
        strlcpy(version, ll.value->valuestring, sizeof(version));
    }
    ll_free(&ll);

    if (lox_struct_loaded()) {
        lox_struct_info_t info;
        lox_struct_get_info(&info);
        if (version[0] && strcmp(info.last_modified, version) == 0) {
            ESP_LOGI(TAG, "Struktur unverändert (%s)", version);
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "Lade LoxAPP3.json (Stand %s)", version[0] ? version : "?");
    int64_t t0 = esp_timer_get_time();
    resp_t r;
    esp_err_t err = request_raw("data/LoxAPP3.json", STRUCT_TIMEOUT_MS, &r);
    if (err != ESP_OK) {
        set_error("Strukturdatei nicht erhalten%s", NULL);
        return err;
    }
    if (r.len < 2 || strncmp(r.data, "{\"LL\"", 5) == 0) {
        heap_caps_free(r.data);
        set_error("Strukturdatei: Fehlerantwort%s", NULL);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%u kB in %lld ms empfangen", (unsigned)(r.len / 1024), (esp_timer_get_time() - t0) / 1000);
    err = lox_struct_load(r.data, r.len);
    if (err == ESP_OK) {
        lox_struct_info_t info;
        lox_struct_get_info(&info);
        portENTER_CRITICAL(&s_lock);
        strlcpy(s_status.ms_name, info.ms_name, sizeof(s_status.ms_name));
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

/* Gesicherte Details aller Türsprechstellen holen (verschlüsselte Abfrage, vor ONLINE) */
static void collect_secured(const cJSON *ctrls, char uuids[][40], int *n)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, ctrls) {
        const cJSON *t = cJSON_GetObjectItem(c, "type");
        const cJSON *u = cJSON_GetObjectItem(c, "uuidAction");
        if (*n < MAX_SECURED && cJSON_IsTrue(cJSON_GetObjectItem(c, "securedDetails")) && cJSON_IsString(u) &&
            cJSON_IsString(t) && strstr(t->valuestring, "Intercom")) {
            strlcpy(uuids[(*n)++], u->valuestring, 40);
        }
        collect_secured(cJSON_GetObjectItem(c, "subControls"), uuids, n);
    }
}

static void load_secured_details(void)
{
    char uuids[MAX_SECURED][40];
    int n = 0;
    if (lox_struct_lock(2000)) {
        collect_secured(cJSON_GetObjectItem(lox_struct_root(), "controls"), uuids, &n);
        lox_struct_unlock();
    }
    for (int i = 0; i < n; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "jdev/sps/io/%.40s/securedDetails", uuids[i]);
        ll_t ll;
        if (request_enc(cmd, &ll) != ESP_OK) continue;
        char *json = NULL;
        if (ll.code == 200 && ll.value) {
            const char *txt = cJSON_IsString(ll.value) ? ll.value->valuestring : NULL;
            char *printed = txt ? NULL : cJSON_PrintUnformatted(ll.value);
            const char *src = txt ? txt : printed;
            if (src) {
                size_t len = strlen(src);
                json = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
                if (json) memcpy(json, src, len + 1);
            }
            cJSON_free(printed);
        }
        ESP_LOGI(TAG, "Gesicherte Details %s: Code %d, %u Bytes", uuids[i], ll.code, json ? (unsigned)strlen(json) : 0);
        if (json) {
            /* Nur Aufbau loggen: Feldnamen und Host/Pfad der Stream-URL, ohne Zugangsdaten/Parameter */
            cJSON *j = cJSON_Parse(json);
            const cJSON *vi = cJSON_GetObjectItem(j, "videoInfo");
            const cJSON *k;
            char keys[160] = "";
            cJSON_ArrayForEach(k, vi) {
                strlcat(keys, k->string, sizeof(keys));
                strlcat(keys, cJSON_IsString(k) && k->valuestring[0] ? "(gesetzt) " : "(leer) ", sizeof(keys));
            }
            const cJSON *su = cJSON_GetObjectItem(vi, "streamUrl");
            char url[128] = "";
            if (cJSON_IsString(su)) {
                const char *p = strstr(su->valuestring, "://");
                p = p ? p + 3 : su->valuestring;
                const char *at = strchr(p, '@');
                const char *sl = strchr(p, '/');
                if (at && (!sl || at < sl)) p = at + 1;     // user:pass@ weglassen
                strlcpy(url, p, sizeof(url));
                char *q = strchr(url, '?');
                if (q) strcpy(q, "?...");
            }
            ESP_LOGI(TAG, "  videoInfo: %s| Stream: %s", keys, url);
            cJSON_Delete(j);
        }
        ll_free(&ll);
        if (json == NULL) continue;
        xSemaphoreTake(s_secured_mx, portMAX_DELAY);
        int slot = -1;
        for (int k = 0; k < s_secured_count; k++) {
            if (!strcmp(s_secured[k].uuid, uuids[i])) slot = k;
        }
        if (slot < 0 && s_secured_count < MAX_SECURED) slot = s_secured_count++;
        char *old = NULL;
        if (slot >= 0) {
            old = s_secured[slot].json;
            strlcpy(s_secured[slot].uuid, uuids[i], sizeof(s_secured[slot].uuid));
            s_secured[slot].json = json;
            json = NULL;
        }
        xSemaphoreGive(s_secured_mx);
        heap_caps_free(old);
        heap_caps_free(json);
    }
}

char *lox_secured_details(const char *uuid_action)
{
    if (s_secured_mx == NULL) return NULL;
    char *out = NULL;
    xSemaphoreTake(s_secured_mx, portMAX_DELAY);
    for (int k = 0; k < s_secured_count; k++) {
        if (!strcasecmp(s_secured[k].uuid, uuid_action) && s_secured[k].json) {
            out = strdup(s_secured[k].json);
            break;
        }
    }
    xSemaphoreGive(s_secured_mx);
    return out;
}

/* ------------------------------------------------------------------ */
/* Sitzung                                                             */

esp_err_t lox_send_command(const char *uuid, const char *cmd)
{
    if (s_ws == NULL || s_status.state != LOX_ONLINE) return ESP_ERR_INVALID_STATE;
    char buf[256];
    snprintf(buf, sizeof(buf), "jdev/sps/io/%s/%s", uuid, cmd);
    ESP_LOGI(TAG, "Befehl %s", buf);
    return send_text(buf);
}

static esp_err_t ws_open(void)
{
    char uri[96];
    snprintf(uri, sizeof(uri), "ws://%s/ws/rfc6455", s_host);
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .subprotocol = "remotecontrol",
        .buffer_size = 8192,
        .task_stack = 6144,
        .task_prio = 5,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 10000,
        .ping_interval_sec = 0,             // Loxone-eigenes Keepalive statt WS-Ping
        .disable_pingpong_discon = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) return ESP_FAIL;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_DISCONNECTED);
    s_expect_payload = false;
    s_out_of_service = false;
    if (esp_websocket_client_start(s_ws) != ESP_OK) return ESP_FAIL;

    EventBits_t b = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_DISCONNECTED, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(15000));
    return (b & BIT_CONNECTED) && !(b & BIT_DISCONNECTED) ? ESP_OK : ESP_FAIL;
}

static void ws_close(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    drain_responses();
}

/* Eine komplette Sitzung. Rückgabe ESP_ERR_INVALID_ARG = Anmeldedaten falsch. */
static esp_err_t run_session(void)
{
    set_state(LOX_CONNECTING);
    ESP_LOGI(TAG, "Verbinde mit ws://%s", s_host);
    if (ws_open() != ESP_OK) {
        set_error("Miniserver nicht erreichbar%s", NULL);
        return ESP_FAIL;
    }

    set_state(LOX_AUTH);
    read_fw_version();
    esp_err_t err = key_exchange();
    if (err == ESP_OK) err = authenticate();
    if (err != ESP_OK) return err;

    set_state(LOX_LOADING);
    err = load_structure();
    if (err != ESP_OK) return err;
    load_secured_details();

    ll_t ll;
    s_log_from_us = esp_timer_get_time() + 5LL * 1000000;    // erster Komplettabzug nicht loggen
    err = request("jdev/sps/enablebinstatusupdate", &ll, REQ_TIMEOUT_MS);
    int code = ll.code;
    ll_free(&ll);
    if (err != ESP_OK || code != 200) {
        set_error("Status-Updates nicht aktiviert%s", NULL);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.state = LOX_ONLINE;
    s_status.sessions++;
    s_status.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Online");

    int64_t last_ka_sent = esp_timer_get_time();
    int64_t last_refresh = esp_timer_get_time();
    s_last_keepalive_us = esp_timer_get_time();
    bool logged_summary = false;

    for (;;) {
        EventBits_t b = xEventGroupWaitBits(s_events, BIT_DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if (b & BIT_DISCONNECTED) {
            set_error(s_out_of_service ? "Miniserver startet neu%s" : "Verbindung getrennt%s", NULL);
            return ESP_FAIL;
        }
        int64_t now = esp_timer_get_time();
        if (!logged_summary && now >= s_log_from_us) {
            logged_summary = true;
            lox_status_t st;
            lox_get_status(&st);
            ESP_LOGI(TAG, "Erster Abzug: %u Werte, %u Texte. Ab jetzt werden Änderungen geloggt.",
                     (unsigned)st.value_events, (unsigned)st.text_events);
        }
        if (now - last_ka_sent > (int64_t)KEEPALIVE_INTERVAL_MS * 1000) {
            last_ka_sent = now;
            send_text("keepalive");
        }
        if (now - s_last_keepalive_us > (int64_t)KEEPALIVE_DEAD_MS * 1000) {
            set_error("Keine Antwort auf Keepalive%s", NULL);
            return ESP_FAIL;
        }
        if (now - last_refresh > REFRESH_INTERVAL_US) {
            last_refresh = now;
            refresh_token();
        }
    }
}

static void lox_task(void *arg)
{
    int backoff = BACKOFF_MIN_S;
    for (;;) {
        uint32_t sessions_before = s_status.sessions;
        esp_err_t err = run_session();
        ws_close();

        /* War diese Sitzung online, nach dem Abbruch wieder kurz warten */
        if (s_status.sessions != sessions_before) {
            backoff = BACKOFF_MIN_S;
        }
        int wait = backoff;
        if (err == ESP_ERR_INVALID_ARG) {
            set_state(LOX_AUTH_FAILED);
            wait = AUTH_FAIL_WAIT_S;
        } else {
            set_state(LOX_OFFLINE);
            backoff = backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff * 2;
        }
        ESP_LOGI(TAG, "Neuer Versuch in %d s", wait);
        for (int i = wait; i > 0; i--) {
            portENTER_CRITICAL(&s_lock);
            s_status.retry_in_s = i;
            portEXIT_CRITICAL(&s_lock);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        portENTER_CRITICAL(&s_lock);
        s_status.retry_in_s = 0;
        portEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t lox_start(const char *host, const char *user, const char *password)
{
    if (host == NULL || host[0] == '\0' || user == NULL || user[0] == '\0') {
        set_state(LOX_IDLE);
        ESP_LOGW(TAG, "Miniserver nicht eingerichtet");
        return ESP_OK;
    }
    strlcpy(s_host, host, sizeof(s_host));
    strlcpy(s_user, user, sizeof(s_user));
    strlcpy(s_pass, password ? password : "", sizeof(s_pass));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_client_uuid, sizeof(s_client_uuid), "4c6f7850-616e-656c-ffff%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (lox_crypto_init() != ESP_OK) return ESP_FAIL;
    lox_struct_lock(0);         // legt den Struktur-Mutex an, bevor mehrere Tasks zugreifen
    lox_struct_unlock();

    /* Selbsttest mit Normvektoren (FIPS 180, RFC 2202/4231) */
    {
        char h[65];
        const uint8_t jefe[] = "Jefe";
        bool ok = true;
        lox_hash_hex(false, "abc", h, false);
        ok &= strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0;
        lox_hash_hex(true, "abc", h, false);
        ok &= strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0;
        lox_hmac_hex(false, jefe, 4, "what do ya want for nothing?", h);
        ok &= strcmp(h, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79") == 0;
        lox_hmac_hex(true, jefe, 4, "what do ya want for nothing?", h);
        ok &= strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0;
        ESP_LOGI(TAG, "Krypto-Selbsttest: %s", ok ? "OK" : "FEHLER");
    }
    s_events = xEventGroupCreate();
    s_resp_q = xQueueCreate(4, sizeof(resp_t));
    if (s_secured_mx == NULL) s_secured_mx = xSemaphoreCreateMutex();
    if (s_events == NULL || s_resp_q == NULL) return ESP_ERR_NO_MEM;

    if (xTaskCreate(lox_task, "lox", 8192, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

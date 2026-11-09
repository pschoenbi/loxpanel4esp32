/*
 * HTTP-Server der Weboberfläche. Eine eingebettete Seite (www/index.html) spricht mit /api/...
 */

#include "web.h"

#include <stdio.h>
#include <string.h>
#include "board.h"
#include "cJSON.h"
#include "dev_settings.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lox_client.h"
#include "lox_struct.h"
#include "mbedtls/base64.h"
#include "mdns.h"
#include "net_wifi.h"
#include "panel_config.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

void web_dns_start(void);

static bool s_setup;
#define MAX_BODY (96 * 1024)

/* ---------------- Hilfen ---------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, txt ? txt : "{}");
    cJSON_free(txt);
    return r;
}

static esp_err_t send_ok(httpd_req_t *req, bool ok, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    if (msg) cJSON_AddStringToObject(j, "msg", msg);
    if (!ok) httpd_resp_set_status(req, "400 Bad Request");
    return send_json(req, j);
}

/* Rumpf lesen (PSRAM, nullterminiert); NULL bei Fehler */
static char *read_body(httpd_req_t *req, size_t *len)
{
    if (req->content_len > MAX_BODY) return NULL;
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (buf == NULL) return NULL;
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            heap_caps_free(buf);
            return NULL;
        }
        got += n;
    }
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

/* Basic Auth: Benutzer egal, Passwort gegen Einstellungen. Im Einrichtungsmodus offen. */
static bool authorized(httpd_req_t *req)
{
    if (s_setup) return true;
    if (!settings_has_web_password()) return false;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;
    unsigned char dec[120];
    size_t dl = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dl, (const unsigned char *)hdr + 6, strlen(hdr + 6)) != 0) {
        return false;
    }
    dec[dl] = '\0';
    char *colon = strchr((char *)dec, ':');
    return colon && settings_check_web_password(colon + 1);
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    if (settings_has_web_password()) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"LoxPanel\"");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, settings_has_web_password() ? "{\"error\":\"auth\"}" : "{\"error\":\"need_password\"}");
}

#define REQUIRE_AUTH(req) do { if (!authorized(req)) return deny(req); } while (0)

static void reboot_cb(void *arg)
{
    esp_restart();
}

static void reboot_later(void)
{
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 800 * 1000);
}

/* ---------------- Seiten ---------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

/* Unbekannte Pfade: im Einrichtungsmodus auf die Startseite umleiten (Captive Portal) */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    if (s_setup) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Nicht gefunden");
    return ESP_FAIL;
}

static esp_err_t h_info(httpd_req_t *req)
{
    net_wifi_status_t w;
    net_wifi_get_status(&w);
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", s_setup ? "setup" : "lan");
    cJSON_AddStringToObject(j, "hostname", w.hostname);
    cJSON_AddStringToObject(j, "ap_ssid", net_wifi_ap_ssid());
    cJSON_AddBoolToObject(j, "need_password", !s_setup && !settings_has_web_password());
    cJSON_AddStringToObject(j, "fw", app->version);
    cJSON_AddStringToObject(j, "board", board_info()->name);
    /* Standard-Raster wie auf dem Panel: 4x3 bei 800 px, 5x3 bei 1024 px Breite */
    int cols = (board_info()->width + 100) / 205;
    cJSON *g = cJSON_AddArrayToObject(j, "grid");
    cJSON_AddItemToArray(g, cJSON_CreateNumber(cols < 2 ? 2 : cols));
    cJSON_AddItemToArray(g, cJSON_CreateNumber(3));
    return send_json(req, j);
}

static esp_err_t h_status(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    net_wifi_status_t w;
    net_wifi_get_status(&w);
    lox_status_t l;
    lox_get_status(&l);
    lox_struct_info_t si = { 0 };
    if (lox_struct_loaded()) lox_struct_get_info(&si);
    const esp_app_desc_t *app = esp_app_get_description();
    const board_info_t *b = board_info();

    cJSON *j = cJSON_CreateObject();
    cJSON *jw = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(jw, "state", net_wifi_state_text(w.state));
    cJSON_AddStringToObject(jw, "ssid", w.ssid);
    cJSON_AddStringToObject(jw, "ip", w.ip);
    cJSON_AddNumberToObject(jw, "rssi", w.rssi);
    cJSON_AddNumberToObject(jw, "channel", w.channel);
    cJSON_AddNumberToObject(jw, "disconnects", w.disconnects);
    cJSON_AddNumberToObject(jw, "roams", w.roams);
    cJSON_AddStringToObject(jw, "c6_fw", w.c6_fw);

    cJSON *jl = cJSON_AddObjectToObject(j, "loxone");
    cJSON_AddStringToObject(jl, "state", lox_state_text(l.state));
    cJSON_AddStringToObject(jl, "ms_name", l.ms_name);
    cJSON_AddStringToObject(jl, "fw", l.fw_version);
    cJSON_AddStringToObject(jl, "hash", l.hash_alg);
    cJSON_AddBoolToObject(jl, "token", lox_has_token());
    cJSON_AddNumberToObject(jl, "rooms", si.rooms);
    cJSON_AddNumberToObject(jl, "controls", si.controls);
    cJSON_AddNumberToObject(jl, "values", l.value_events);
    cJSON_AddStringToObject(jl, "error", l.last_error);

    cJSON *js = cJSON_AddObjectToObject(j, "system");
    cJSON_AddStringToObject(js, "fw", app->version);
    cJSON_AddStringToObject(js, "idf", app->idf_ver);
    cJSON_AddStringToObject(js, "board", b->name);
    cJSON_AddNumberToObject(js, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(js, "heap_int_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(js, "heap_psram_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    cJSON_AddBoolToObject(js, "cfg_backup", cfg_has_backup());
    return send_json(req, j);
}

static esp_err_t h_settings_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    dev_settings_t s;
    settings_get(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_ssid", s.wifi_ssid);
    cJSON_AddBoolToObject(j, "wifi_pass_set", s.wifi_pass[0] != 0);
    cJSON_AddStringToObject(j, "ip", s.ip);
    cJSON_AddStringToObject(j, "mask", s.mask);
    cJSON_AddStringToObject(j, "gw", s.gw);
    cJSON_AddStringToObject(j, "mac", s.mac);
    cJSON_AddStringToObject(j, "ms_host", s.ms_host);
    cJSON_AddStringToObject(j, "ms_user", s.ms_user);
    cJSON_AddBoolToObject(j, "ms_pass_set", s.ms_pass[0] != 0);
    cJSON_AddBoolToObject(j, "ms_token", lox_has_token());
    cJSON_AddBoolToObject(j, "web_pass_set", settings_has_web_password());
    return send_json(req, j);
}

static void take_str(const cJSON *j, const char *k, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItem(j, k);
    if (cJSON_IsString(v)) strlcpy(out, v->valuestring, size);
}

/* Passwortfelder: leer = unverändert lassen */
static void take_secret(const cJSON *j, const char *k, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItem(j, k);
    if (cJSON_IsString(v) && v->valuestring[0]) strlcpy(out, v->valuestring, size);
}

static esp_err_t h_settings_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *body = read_body(req, NULL);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    heap_caps_free(body);
    if (j == NULL) return send_ok(req, false, "Ungültige Anfrage");

    dev_settings_t s;
    settings_get(&s);
    char old_host[64], old_user[64];
    strlcpy(old_host, s.ms_host, sizeof(old_host));
    strlcpy(old_user, s.ms_user, sizeof(old_user));

    take_str(j, "wifi_ssid", s.wifi_ssid, sizeof(s.wifi_ssid));
    take_secret(j, "wifi_pass", s.wifi_pass, sizeof(s.wifi_pass));
    take_str(j, "ip", s.ip, sizeof(s.ip));
    take_str(j, "mask", s.mask, sizeof(s.mask));
    take_str(j, "gw", s.gw, sizeof(s.gw));
    take_str(j, "mac", s.mac, sizeof(s.mac));
    take_str(j, "ms_host", s.ms_host, sizeof(s.ms_host));
    take_str(j, "ms_user", s.ms_user, sizeof(s.ms_user));
    take_secret(j, "ms_pass", s.ms_pass, sizeof(s.ms_pass));

    const cJSON *wp = cJSON_GetObjectItem(j, "web_pass");
    cJSON_Delete(j);
    if (!s.wifi_ssid[0]) return send_ok(req, false, "WLAN-Name fehlt");

    esp_err_t err = settings_save(&s);
    if (err == ESP_OK && cJSON_IsString(wp) && wp->valuestring[0]) {
        err = settings_set_web_password(wp->valuestring);
        if (err != ESP_OK) return send_ok(req, false, "Web-Passwort zu kurz (mindestens 4 Zeichen)");
    }
    /* Neuer Miniserver/Benutzer: altes Token gilt nicht mehr */
    if (strcmp(old_host, s.ms_host) || strcmp(old_user, s.ms_user)) lox_forget_token();
    return send_ok(req, err == ESP_OK, err == ESP_OK ? "Gespeichert" : "Speichern fehlgeschlagen");
}

static esp_err_t h_password(httpd_req_t *req)
{
    /* Erstes Setzen ohne Anmeldung möglich, danach nur angemeldet */
    if (settings_has_web_password()) REQUIRE_AUTH(req);
    char *body = read_body(req, NULL);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    heap_caps_free(body);
    const cJSON *pw = cJSON_GetObjectItem(j, "web_pass");
    esp_err_t err = cJSON_IsString(pw) ? settings_set_web_password(pw->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    return send_ok(req, err == ESP_OK, err == ESP_OK ? "Passwort gesetzt" : "Mindestens 4 Zeichen");
}

static esp_err_t h_scan(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    net_wifi_ap_t aps[24];
    int n = net_wifi_scan(aps, 24);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, arr);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    size_t len = 0;
    char *txt = cfg_read_text(&len);
    if (txt == NULL) return send_ok(req, false, "Keine Konfiguration");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"panel.json\"");
    esp_err_t r = httpd_resp_send(req, txt, len);
    heap_caps_free(txt);
    return r;
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    size_t len = 0;
    char *body = read_body(req, &len);
    if (body == NULL) return send_ok(req, false, "Datei zu gross oder Übertragung fehlgeschlagen");
    char err[96] = "";
    esp_err_t e = cfg_import(body, len, err, sizeof(err));
    heap_caps_free(body);
    return send_ok(req, e == ESP_OK, e == ESP_OK ? "Übernommen" : err);
}

static esp_err_t h_config_undo(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    esp_err_t e = cfg_undo();
    return send_ok(req, e == ESP_OK, e == ESP_OK ? "Vorherige Konfiguration wiederhergestellt" : "Kein Backup vorhanden");
}

/* Bausteine der Loxone-Struktur für den Editor (inkl. Unterbausteine) */
static void add_controls(cJSON *arr, const cJSON *ctrls, const char *parent_room, const char *parent_name,
                         const cJSON *cats)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, ctrls) {
        const char *room = lox_struct_room_of(c);
        if (room == NULL) room = parent_room;
        const cJSON *name = cJSON_GetObjectItem(c, "name");
        const cJSON *type = cJSON_GetObjectItem(c, "type");
        const cJSON *uuid = cJSON_GetObjectItem(c, "uuidAction");
        const cJSON *cat = cJSON_GetObjectItem(c, "cat");
        const cJSON *cn = cJSON_IsString(cat) ? cJSON_GetObjectItem(cJSON_GetObjectItem(cats, cat->valuestring), "name") : NULL;
        if (cJSON_IsString(uuid) && cJSON_IsString(type)) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "u", uuid->valuestring);
            cJSON_AddStringToObject(o, "n", cJSON_IsString(name) ? name->valuestring : "?");
            cJSON_AddStringToObject(o, "r", room ? room : "");
            cJSON_AddStringToObject(o, "c", cJSON_IsString(cn) ? cn->valuestring : "");
            cJSON_AddStringToObject(o, "t", type->valuestring);
            if (cJSON_IsTrue(cJSON_GetObjectItem(c, "isFavorite"))) cJSON_AddTrueToObject(o, "f");
            if (parent_name) cJSON_AddStringToObject(o, "p", parent_name);
            cJSON_AddItemToArray(arr, o);
        }
        add_controls(arr, cJSON_GetObjectItem(c, "subControls"), room,
                     cJSON_IsString(name) ? name->valuestring : "", cats);
    }
}

static esp_err_t h_controls(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (!lox_struct_loaded() || !lox_struct_lock(2000)) {
        return send_ok(req, false, "Struktur des Miniservers noch nicht geladen");
    }
    const cJSON *root = lox_struct_root();
    cJSON *j = cJSON_CreateObject();
    lox_struct_info_t si;
    lox_struct_get_info(&si);
    cJSON_AddStringToObject(j, "ms", si.ms_name);
    cJSON *rooms = cJSON_AddArrayToObject(j, "rooms");
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(root, "rooms")) {
        const cJSON *n = cJSON_GetObjectItem(r, "name");
        if (cJSON_IsString(n)) cJSON_AddItemToArray(rooms, cJSON_CreateString(n->valuestring));
    }
    cJSON *arr = cJSON_AddArrayToObject(j, "controls");
    add_controls(arr, cJSON_GetObjectItem(root, "controls"), NULL, NULL, cJSON_GetObjectItem(root, "cats"));
    lox_struct_unlock();
    return send_json(req, j);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_ok(req, true, "Neustart");
    reboot_later();
    return ESP_OK;
}

/* ---------------- Start ---------------- */

static void mdns_start(void)
{
    net_wifi_status_t w;
    net_wifi_get_status(&w);
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(w.hostname);
    mdns_instance_name_set("LoxPanel");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_txt_item_t txt[] = { { "board", board_info()->name }, { "fw", esp_app_get_description()->version } };
    mdns_service_add("LoxPanel", "_loxpanel", "_tcp", 80, txt, 2);
    ESP_LOGI(TAG, "mDNS: http://%s.local", w.hostname);
}

esp_err_t web_start(bool setup_mode)
{
    s_setup = setup_mode;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t srv;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",                 .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/info",         .method = HTTP_GET,  .handler = h_info },
        { .uri = "/api/status",       .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/settings",     .method = HTTP_GET,  .handler = h_settings_get },
        { .uri = "/api/settings",     .method = HTTP_POST, .handler = h_settings_post },
        { .uri = "/api/password",     .method = HTTP_POST, .handler = h_password },
        { .uri = "/api/scan",         .method = HTTP_GET,  .handler = h_scan },
        { .uri = "/api/config",       .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/config",       .method = HTTP_POST, .handler = h_config_post },
        { .uri = "/api/config/undo",  .method = HTTP_POST, .handler = h_config_undo },
        { .uri = "/api/reboot",       .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/api/controls",     .method = HTTP_GET,  .handler = h_controls },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, h_404);

    if (setup_mode) {
        web_dns_start();
    } else {
        mdns_start();
    }
    ESP_LOGI(TAG, "Weboberfläche aktiv (%s)", setup_mode ? "Einrichtung" : "LAN");
    return ESP_OK;
}

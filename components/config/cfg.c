/*
 * Display-Konfiguration: Dateisystem, Vorlage, Laden, Platzhalter, Abweichungen, Auflösung.
 *
 * Vorlage: config/panel.json aus dem Projekt ist in die Firmware eingebettet. Ändert sie sich
 * (CRC), wird sie nach /config/panel.json übernommen; die auf dem Gerät gesetzten
 * "abweichungen" bleiben erhalten (wie Vorlagen-Verteilung in SPEC 5.3).
 */

#include "panel_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "lox_struct.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "cfg";

#define CFG_BAK     "/config/panel.bak"
#define CFG_TMP     "/config/panel.tmp"

extern const uint8_t tpl_start[] asm("_binary_panel_json_start");
extern const uint8_t tpl_end[]   asm("_binary_panel_json_end");

static cJSON *s_doc;                // aktuelle Datei als JSON
static panel_cfg_t s_cfg;
static uint32_t s_generation = 1;
#if CONFIG_LOXPANEL_CFG_DUMP
static bool s_dumped;
#endif

/* Typen, die auf Auto-Seiten erscheinen: nur was die Oberfläche bedienen/anzeigen kann.
   Raumregler (IRoomControllerV2) folgt mit Schritt 7. */
static const char *const AUTO_TYPES[] = {
    "Switch", "Pushbutton", "Dimmer", "EIBDimmer", "Jalousie", "LightControllerV2",
    "IRoomControllerV2", "EFM", "Intercom", "IntercomV2", "Gate", "NfcCodeTouch", "WindowMonitor", "Meter", "InfoOnlyAnalog", "InfoOnlyDigital", "TextState", NULL,
};

/* ------------------------------------------------------------------ */
/* Dateien                                                             */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (buf && fread(buf, 1, n, f) == (size_t)n) {
        buf[n] = '\0';
        if (len) *len = n;
    } else {
        heap_caps_free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

/* Schreiben über Temp-Datei + rename, damit ein Stromausfall nie eine halbe Datei hinterlässt */
static esp_err_t write_json(const char *path, const cJSON *doc)
{
    char *txt = cJSON_Print(doc);
    if (txt == NULL) return ESP_ERR_NO_MEM;
    FILE *f = fopen(CFG_TMP, "wb");
    esp_err_t err = ESP_FAIL;
    if (f) {
        size_t n = strlen(txt);
        if (fwrite(txt, 1, n, f) == n) err = ESP_OK;
        fclose(f);
    }
    cJSON_free(txt);
    if (err == ESP_OK) {
        remove(path);
        if (rename(CFG_TMP, path) != 0) err = ESP_FAIL;
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "%s nicht geschrieben", path);
    return err;
}

static uint32_t nvs_get_crc(void)
{
    uint32_t v = 0;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "tpl_crc", &v);
        nvs_close(h);
    }
    return v;
}

static void nvs_set_crc(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "tpl_crc", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Vorlage übernehmen, falls neu. Abweichungen des Geräts bleiben. */
static void apply_template_if_changed(void)
{
    size_t tlen = tpl_end - tpl_start;
    uint32_t crc = esp_rom_crc32_le(0, tpl_start, tlen);
    struct stat st;
    bool have_file = stat(CFG_PATH, &st) == 0;
    if (have_file && crc == nvs_get_crc()) return;

    cJSON *tpl = cJSON_ParseWithLength((const char *)tpl_start, tlen);
    if (tpl == NULL) {
        ESP_LOGE(TAG, "Eingebettete Vorlage ist kein gültiges JSON");
        return;
    }
    if (have_file) {
        char *old = read_file(CFG_PATH, NULL);
        cJSON *od = old ? cJSON_Parse(old) : NULL;
        heap_caps_free(old);
        const cJSON *ab = od ? cJSON_GetObjectItem(od, "abweichungen") : NULL;
        if (ab) {
            cJSON_DeleteItemFromObject(tpl, "abweichungen");
            cJSON_AddItemToObject(tpl, "abweichungen", cJSON_Duplicate(ab, true));
        }
        if (od) {
            write_json(CFG_BAK, od);    // eine Stufe Rückgängig
            cJSON_Delete(od);
        }
    }
    if (write_json(CFG_PATH, tpl) == ESP_OK) {
        nvs_set_crc(crc);
        ESP_LOGI(TAG, "Vorlage übernommen (%u Bytes)%s", (unsigned)tlen,
                 have_file ? ", Abweichungen beibehalten" : "");
    }
    cJSON_Delete(tpl);
}

/* ------------------------------------------------------------------ */
/* Modell                                                              */

/* "{name}" durch Variablen ersetzen */
static void expand(const char *in, char *out, size_t size)
{
    const cJSON *vars = cJSON_GetObjectItem(s_doc, "variablen");
    size_t o = 0;
    while (*in && o + 1 < size) {
        if (*in == '{') {
            const char *end = strchr(in, '}');
            if (end && end - in < 48) {
                char name[48];
                memcpy(name, in + 1, end - in - 1);
                name[end - in - 1] = '\0';
                const cJSON *v = cJSON_GetObjectItem(vars, name);
                if (cJSON_IsString(v)) {
                    for (const char *p = v->valuestring; *p && o + 1 < size; p++) out[o++] = *p;
                    in = end + 1;
                    continue;
                }
                ESP_LOGW(TAG, "Variable {%s} nicht definiert", name);
            }
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
}

static int get_int(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static void get_str(const cJSON *o, const char *k, char *out, size_t size, bool do_expand)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    out[0] = '\0';
    if (!cJSON_IsString(v)) return;
    if (do_expand) expand(v->valuestring, out, size);
    else strlcpy(out, v->valuestring, size);
}

static void free_model(void)
{
    for (int i = 0; i < s_cfg.page_count; i++) heap_caps_free(s_cfg.pages[i].tiles);
    heap_caps_free(s_cfg.pages);
    memset(&s_cfg, 0, sizeof(s_cfg));
}

static void build_model(void)
{
    free_model();
    s_cfg.schema = get_int(s_doc, "schema", 0);
    get_str(s_doc, "vorlage", s_cfg.vorlage, sizeof(s_cfg.vorlage), false);
    s_cfg.vorlage_version = get_int(s_doc, "vorlage_version", 0);
    const cJSON *anz = cJSON_GetObjectItem(s_doc, "anzeige");
    s_cfg.helligkeit = get_int(anz, "helligkeit", 80);
    s_cfg.dimmen_nach_s = get_int(anz, "dimmen_nach_s", 60);
    s_cfg.startseite = get_int(anz, "startseite", 0);
    const char *raum = cfg_var("raum");
    strlcpy(s_cfg.raum, raum ? raum : "", sizeof(s_cfg.raum));

    const cJSON *pages = cJSON_GetObjectItem(s_doc, "seiten");
    const cJSON *ab = cJSON_GetObjectItem(s_doc, "abweichungen");
    int np = cJSON_GetArraySize(pages);
    s_cfg.pages = heap_caps_calloc(np ? np : 1, sizeof(cfg_page_t), MALLOC_CAP_SPIRAM);
    if (s_cfg.pages == NULL) return;

    const cJSON *pj;
    cJSON_ArrayForEach(pj, pages) {
        cfg_page_t *p = &s_cfg.pages[s_cfg.page_count++];
        get_str(pj, "titel", p->titel, sizeof(p->titel), true);
        if (!p->titel[0]) snprintf(p->titel, sizeof(p->titel), "Seite %d", s_cfg.page_count);
        const cJSON *r = cJSON_GetObjectItem(pj, "raster");
        if (cJSON_GetArraySize(r) == 2) {
            p->cols = cJSON_GetArrayItem(r, 0)->valueint;
            p->rows = cJSON_GetArrayItem(r, 1)->valueint;
        }
        const cJSON *au = cJSON_GetObjectItem(pj, "auto");
        if (au) {
            p->is_auto = true;
            get_str(au, "raum", p->auto_raum, sizeof(p->auto_raum), true);
            p->auto_favoriten = cJSON_IsTrue(cJSON_GetObjectItem(au, "favoriten"));
            continue;       // Kacheln entstehen erst bei der Auflösung
        }

        const cJSON *tiles = cJSON_GetObjectItem(pj, "kacheln");
        int nt = cJSON_GetArraySize(tiles);
        p->tiles = heap_caps_calloc(nt ? nt : 1, sizeof(cfg_tile_t), MALLOC_CAP_SPIRAM);
        int idx = 0;
        const cJSON *tj;
        cJSON_ArrayForEach(tj, tiles) {
            idx++;
            char key[72];
            snprintf(key, sizeof(key), "%s/%d", p->titel, idx);
            const cJSON *ov = cJSON_GetObjectItem(ab, key);
            if (cJSON_IsTrue(cJSON_GetObjectItem(ov, "ausblenden"))) continue;

            cfg_tile_t *t = &p->tiles[p->count++];
            strlcpy(t->key, key, sizeof(t->key));
            get_str(tj, "typ", t->typ, sizeof(t->typ), false);
            if (!t->typ[0]) strcpy(t->typ, "auto");
            get_str(tj, "ziel", t->ziel, sizeof(t->ziel), true);
            if (cJSON_IsString(cJSON_GetObjectItem(ov, "ziel"))) {
                get_str(ov, "ziel", t->ziel, sizeof(t->ziel), true);
            }
            get_str(tj, "titel", t->titel, sizeof(t->titel), true);
            int b = get_int(tj, "breite", 1), h = get_int(tj, "hoehe", 1);
            t->breite = b < 1 ? 1 : b > 8 ? 8 : b;
            t->hoehe = h < 1 ? 1 : h > 8 ? 8 : h;
            t->bind = CFG_BIND_PENDING;
        }
    }
    ESP_LOGI(TAG, "Konfiguration: Vorlage \"%s\" v%d, %d Seiten, Raum \"%s\"",
             s_cfg.vorlage, s_cfg.vorlage_version, s_cfg.page_count, s_cfg.raum);
}

static esp_err_t load_file(void)
{
    size_t len = 0;
    char *txt = read_file(CFG_PATH, &len);
    cJSON *doc = txt ? cJSON_ParseWithLength(txt, len) : NULL;
    heap_caps_free(txt);
    if (doc == NULL) {
        ESP_LOGE(TAG, "%s fehlt oder ist kein gültiges JSON - nutze Vorlage", CFG_PATH);
        doc = cJSON_ParseWithLength((const char *)tpl_start, tpl_end - tpl_start);
    }
    if (doc == NULL) return ESP_FAIL;
    if (get_int(doc, "schema", 0) != CFG_SCHEMA) {
        ESP_LOGE(TAG, "Schema %d wird nicht unterstützt (erwartet %d)", get_int(doc, "schema", 0), CFG_SCHEMA);
    }
    if (s_doc) cJSON_Delete(s_doc);
    s_doc = doc;
    build_model();
    s_generation++;
    return ESP_OK;
}

esp_err_t cfg_init(void)
{
    esp_vfs_littlefs_conf_t fs = {
        .base_path = "/config",
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Dateisystem nicht eingehängt: %s", esp_err_to_name(err));
    } else {
        size_t total = 0, used = 0;
        esp_littlefs_info("storage", &total, &used);
        ESP_LOGI(TAG, "/config: %u von %u kB belegt", (unsigned)(used / 1024), (unsigned)(total / 1024));
        apply_template_if_changed();
    }
    return load_file();
}

const panel_cfg_t *cfg_get(void)
{
    return &s_cfg;
}

uint32_t cfg_generation(void)
{
    return s_generation;
}

const char *cfg_var(const char *name)
{
    const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(s_doc, "variablen"), name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

esp_err_t cfg_set_override(const char *key, const char *ziel, bool hide)
{
    if (s_doc == NULL) return ESP_ERR_INVALID_STATE;
    cJSON *ab = cJSON_GetObjectItem(s_doc, "abweichungen");
    if (ab == NULL) ab = cJSON_AddObjectToObject(s_doc, "abweichungen");
    cJSON *ov = cJSON_GetObjectItem(ab, key);
    if (ov == NULL) ov = cJSON_AddObjectToObject(ab, key);
    if (ziel) {
        cJSON_DeleteItemFromObject(ov, "ziel");
        cJSON_AddStringToObject(ov, "ziel", ziel);
    }
    if (hide) {
        cJSON_DeleteItemFromObject(ov, "ausblenden");
        cJSON_AddTrueToObject(ov, "ausblenden");
    }
    esp_err_t err = write_json(CFG_PATH, s_doc);
    ESP_LOGI(TAG, "Abweichung %s: %s%s", key, ziel ? ziel : "", hide ? " ausgeblendet" : "");
    build_model();
    s_generation++;
    return err;
}

/* ------------------------------------------------------------------ */
/* Import / Undo (Weboberfläche)                                       */

static volatile bool s_reload_pending;

char *cfg_read_text(size_t *len)
{
    return read_file(CFG_PATH, len);
}

static esp_err_t write_text(const char *path, const char *txt, size_t len)
{
    FILE *f = fopen(CFG_TMP, "wb");
    if (f == NULL) return ESP_FAIL;
    size_t n = fwrite(txt, 1, len, f);
    fclose(f);
    if (n != len) return ESP_FAIL;
    remove(path);
    return rename(CFG_TMP, path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t cfg_import(const char *text, size_t len, char *err, size_t err_size)
{
    cJSON *doc = cJSON_ParseWithLength(text, len);
    if (doc == NULL) {
        snprintf(err, err_size, "Kein gültiges JSON (Fehler nahe: %.30s)", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        return ESP_ERR_INVALID_ARG;
    }
    int schema = get_int(doc, "schema", 0);
    const cJSON *pages = cJSON_GetObjectItem(doc, "seiten");
    esp_err_t ret = ESP_OK;
    if (schema != CFG_SCHEMA) {
        snprintf(err, err_size, "Schema %d wird nicht unterstützt (erwartet %d)", schema, CFG_SCHEMA);
        ret = ESP_ERR_INVALID_VERSION;
    } else if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) == 0) {
        snprintf(err, err_size, "\"seiten\" fehlt oder ist leer");
        ret = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(doc);
    if (ret != ESP_OK) return ret;

    /* Backup der bisherigen Datei (eine Stufe Rückgängig) */
    size_t olen = 0;
    char *old = read_file(CFG_PATH, &olen);
    if (old) {
        write_text(CFG_BAK, old, olen);
        heap_caps_free(old);
    }
    if (write_text(CFG_PATH, text, len) != ESP_OK) {
        snprintf(err, err_size, "Speichern fehlgeschlagen");
        return ESP_FAIL;
    }
    /* Die eingebettete Vorlage gilt damit als übernommen und überschreibt den Import nicht */
    nvs_set_crc(esp_rom_crc32_le(0, tpl_start, tpl_end - tpl_start));
    s_reload_pending = true;
    ESP_LOGI(TAG, "Konfiguration importiert (%u Bytes)", (unsigned)len);
    return ESP_OK;
}

bool cfg_has_backup(void)
{
    struct stat st;
    return stat(CFG_BAK, &st) == 0;
}

esp_err_t cfg_undo(void)
{
    size_t blen = 0, clen = 0;
    char *bak = read_file(CFG_BAK, &blen);
    if (bak == NULL) return ESP_ERR_NOT_FOUND;
    char *cur = read_file(CFG_PATH, &clen);
    esp_err_t err = write_text(CFG_PATH, bak, blen);
    if (err == ESP_OK && cur) write_text(CFG_BAK, cur, clen);     // Undo ist selbst rückgängig machbar
    heap_caps_free(bak);
    heap_caps_free(cur);
    if (err == ESP_OK) {
        s_reload_pending = true;
        ESP_LOGI(TAG, "Vorherige Konfiguration wiederhergestellt");
    }
    return err;
}

void cfg_poll(void)
{
    if (s_reload_pending) {
        s_reload_pending = false;
        load_file();
    }
}

/* ------------------------------------------------------------------ */
/* Auflösung gegen die Loxone-Struktur                                 */

/* Kacheltyp -> passende Loxone-Typen (NULL-terminiert); "auto" = alle */
static const char *const *types_for(const char *typ)
{
    static const char *const schalter[] = { "Switch", "Pushbutton", NULL };
    static const char *const dimmer[] = { "Dimmer", "EIBDimmer", NULL };
    static const char *const jalousie[] = { "Jalousie", NULL };
    static const char *const licht[] = { "LightControllerV2", NULL };
    static const char *const raumregler[] = { "IRoomControllerV2", NULL };
    static const char *const zaehler[] = { "Meter", NULL };
    static const char *const energie[] = { "EFM", NULL };
    static const char *const kamera[] = { "Intercom", "IntercomV2", NULL };
    static const char *const tuer[] = { "Intercom", "IntercomV2", "Gate", "NfcCodeTouch", "WindowMonitor", NULL };
    static const char *const info[] = { "InfoOnlyAnalog", "InfoOnlyDigital", "TextState", NULL };
    if (!strcmp(typ, "schalter")) return schalter;
    if (!strcmp(typ, "dimmer")) return dimmer;
    if (!strcmp(typ, "jalousie")) return jalousie;
    if (!strcmp(typ, "licht")) return licht;
    if (!strcmp(typ, "raumregler")) return raumregler;
    if (!strcmp(typ, "zaehler")) return zaehler;
    if (!strcmp(typ, "energie")) return energie;
    if (!strcmp(typ, "kamera")) return kamera;
    if (!strcmp(typ, "tuer")) return tuer;
    if (!strcmp(typ, "info")) return info;
    return NULL;
}

typedef struct {
    const char *room;           // gesuchter Raum (NULL = egal)
    const char *name;           // gesuchter Name (NULL = egal)
    const char *uuid;           // gesuchte uuidAction (NULL = egal)
    const char *const *types;   // erlaubte Loxone-Typen (NULL = alle)
    bool favorites;
    int hits;
    char first_uuid[48];
    int top_hits;               // Treffer unter den Hauptbausteinen (nicht Unterbausteine)
    char first_top_uuid[48];
    int depth;
    /* Sammeln für Auto-Seiten */
    cfg_page_t *collect;
    int cap;
} search_t;

static bool name_eq(const char *a, const char *b)
{
    /* Gross/klein egal, Leerzeichen am Rand egal */
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == ' ') la--;
    while (lb && b[lb - 1] == ' ') lb--;
    return la == lb && strncasecmp(a, b, la) == 0;
}

static bool is_auto_type(const char *t)
{
    for (int i = 0; AUTO_TYPES[i]; i++) {
        if (t && !strcmp(t, AUTO_TYPES[i])) return true;
    }
    return false;
}

static void walk(const cJSON *ctrls, const char *parent_room, search_t *s)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, ctrls) {
        const char *room = lox_struct_room_of(c);
        if (room == NULL) room = parent_room;
        const cJSON *name = cJSON_GetObjectItem(c, "name");
        const cJSON *uuid = cJSON_GetObjectItem(c, "uuidAction");
        const cJSON *type = cJSON_GetObjectItem(c, "type");
        const char *n = cJSON_IsString(name) ? name->valuestring : "";
        const char *u = cJSON_IsString(uuid) ? uuid->valuestring : "";

        bool match;
        if (s->uuid) {
            match = !strcmp(u, s->uuid);
        } else if (s->collect) {
            match = is_auto_type(cJSON_IsString(type) ? type->valuestring : NULL) &&
                    (s->favorites ? cJSON_IsTrue(cJSON_GetObjectItem(c, "isFavorite"))
                                  : (room && name_eq(room, s->room)));
        } else {
            match = room && name_eq(room, s->room) && name_eq(n, s->name);
            if (match && s->types) {
                const char *tt = cJSON_IsString(type) ? type->valuestring : "";
                bool ok = false;
                for (int i = 0; s->types[i]; i++) ok |= !strcmp(tt, s->types[i]);
                match = ok;
            }
        }

        if (match) {
            if (s->hits++ == 0) strlcpy(s->first_uuid, u, sizeof(s->first_uuid));
            if (s->depth == 0 && s->top_hits++ == 0) strlcpy(s->first_top_uuid, u, sizeof(s->first_top_uuid));
            if (s->collect) {
                cfg_page_t *p = s->collect;
                if (p->count == s->cap) {
                    s->cap = s->cap ? s->cap * 2 : 16;
                    p->tiles = heap_caps_realloc(p->tiles, s->cap * sizeof(cfg_tile_t), MALLOC_CAP_SPIRAM);
                }
                if (p->tiles) {
                    cfg_tile_t *t = &p->tiles[p->count++];
                    memset(t, 0, sizeof(*t));
                    snprintf(t->key, sizeof(t->key), "%s/%s", p->titel, n);
                    /* auf dem Gerät ausgeblendet? */
                    const cJSON *ov = cJSON_GetObjectItem(cJSON_GetObjectItem(s_doc, "abweichungen"), t->key);
                    if (cJSON_IsTrue(cJSON_GetObjectItem(ov, "ausblenden"))) {
                        p->count--;
                        goto next;
                    }
                    t->from_auto = true;
                    strcpy(t->typ, "auto");
                    snprintf(t->ziel, sizeof(t->ziel), "uuid:%s", u);
                    t->breite = t->hoehe = 1;
                    t->bind = CFG_BIND_OK;
                    strlcpy(t->uuid, u, sizeof(t->uuid));
                }
            }
        }
next:
        /* Unterbausteine (z. B. Ausgänge einer Lichtsteuerung) nur bei gezielter Suche */
        if (!s->collect) {
            s->depth++;
            walk(cJSON_GetObjectItem(c, "subControls"), room, s);
            s->depth--;
        }
    }
}

#if CONFIG_LOXPANEL_CFG_DUMP
static void dump_controls(const cJSON *ctrls, const char *parent_room)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, ctrls) {
        const char *room = lox_struct_room_of(c);
        if (room == NULL) room = parent_room;
        const cJSON *name = cJSON_GetObjectItem(c, "name");
        const cJSON *type = cJSON_GetObjectItem(c, "type");
        if (cJSON_IsString(type)) {
            /* "-" = Typ wird von der Oberfläche (noch) nicht unterstützt */
            ESP_LOGI(TAG, "  %s %s/%s [%s]%s", is_auto_type(type->valuestring) ? " " : "-", room ? room : "-",
                     cJSON_IsString(name) ? name->valuestring : "?", type->valuestring,
                     cJSON_IsTrue(cJSON_GetObjectItem(c, "isFavorite")) ? " *" : "");
        }
        dump_controls(cJSON_GetObjectItem(c, "subControls"), room);
    }
}
#endif

void cfg_resolve(void)
{
    const cJSON *ctrls = cJSON_GetObjectItem(lox_struct_root(), "controls");
    if (ctrls == NULL) return;

#if CONFIG_LOXPANEL_CFG_DUMP
    if (!s_dumped) {
        s_dumped = true;
        ESP_LOGI(TAG, "Bausteine (Raum/Name [Typ], * = Favorit):");
        dump_controls(ctrls, NULL);
    }
#endif

    for (int i = 0; i < s_cfg.page_count; i++) {
        cfg_page_t *p = &s_cfg.pages[i];
        if (p->is_auto) {
            heap_caps_free(p->tiles);
            p->tiles = NULL;
            p->count = 0;
            search_t s = { .room = p->auto_raum, .favorites = p->auto_favoriten, .collect = p };
            walk(ctrls, NULL, &s);
            ESP_LOGI(TAG, "Seite \"%s\": %d Bausteine automatisch", p->titel, p->count);
            continue;
        }
        for (int k = 0; k < p->count; k++) {
            cfg_tile_t *t = &p->tiles[k];
            search_t s = { 0 };
            char room[112];
            if (!strncmp(t->ziel, "uuid:", 5)) {
                s.uuid = t->ziel + 5;
            } else {
                strlcpy(room, t->ziel, sizeof(room));
                char *slash = strchr(room, '/');
                if (slash == NULL) {
                    t->bind = CFG_BIND_MISSING;
                    continue;
                }
                *slash = '\0';
                s.room = room;
                s.name = slash + 1;
                s.types = types_for(t->typ);
            }
            walk(ctrls, NULL, &s);
            if (s.hits == 1) {
                t->bind = CFG_BIND_OK;
                strlcpy(t->uuid, s.first_uuid, sizeof(t->uuid));
            } else if (s.hits > 1 && s.top_hits == 1) {
                /* Mehrere Treffer, aber nur einer ist ein Hauptbaustein: den nehmen */
                t->bind = CFG_BIND_OK;
                strlcpy(t->uuid, s.first_top_uuid, sizeof(t->uuid));
            } else {
                t->bind = s.hits == 0 ? CFG_BIND_MISSING : CFG_BIND_AMBIGUOUS;
                t->uuid[0] = '\0';
            }
            if (t->bind != CFG_BIND_OK) {
                ESP_LOGW(TAG, "Kachel %s \"%s\": %s", t->key, t->ziel,
                         t->bind == CFG_BIND_MISSING ? "nicht gefunden" : "mehrdeutig");
            }
        }
    }
}

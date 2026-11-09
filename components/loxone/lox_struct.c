/*
 * LoxAPP3.json einlesen und eine sortierte Tabelle Status-UUID -> Baustein aufbauen.
 * Der cJSON-Baum bleibt im PSRAM, die Tabelle verweist auf seine Strings.
 */

#include "lox_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "lox_struct";

static cJSON *s_root;
static lox_state_ref_t *s_states;
static size_t s_state_count;
static size_t s_state_cap;
static lox_struct_info_t s_info;

/* Werte parallel zu s_states */
static double *s_values;
static char **s_texts;
typedef struct {
    double def;
    int n;
    lox_dt_entry_t e[];
} dt_blob_t;
static dt_blob_t **s_dts;           // Zeitpläne (nur für Daytimer-Status belegt)
static portMUX_TYPE s_vlock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_changes;
static uint32_t s_generation;
static SemaphoreHandle_t s_mutex;       // schützt Baum und Tabellen beim Austausch

static void ensure_mutex(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
    }
}

bool lox_struct_lock(uint32_t timeout_ms)
{
    ensure_mutex();
    return xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lox_struct_unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

uint32_t lox_struct_generation(void)
{
    return s_generation;
}

uint32_t lox_value_changes(void)
{
    return s_changes;
}

static void *psram_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(sz);
}

/* ---------- UUID ---------- */

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Format: 8-4-4-16 Hexziffern, z. B. 0f1e2d3c-005f-1234-ffff403fb0c34b9e */
bool lox_uuid_parse(const char *s, lox_uuid_t *out)
{
    if (s == NULL) return false;
    int n = 0;
    for (int i = 0; s[i] && n < 32; i++) {
        if (s[i] == '-') continue;
        int hi = hexv(s[i]);
        int lo = s[i + 1] ? hexv(s[i + 1]) : -1;
        if (hi < 0 || lo < 0) return false;
        out->b[n / 2] = (uint8_t)((hi << 4) | lo);
        n += 2;
        i++;
    }
    return n == 32;
}

void lox_uuid_format(const lox_uuid_t *u, char *out)
{
    const uint8_t *b = u->b;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void lox_uuid_from_wire(const uint8_t *p, lox_uuid_t *out)
{
    /* uint32 d1, uint16 d2, uint16 d3 (little endian), uint8 d4[8] */
    out->b[0] = p[3]; out->b[1] = p[2]; out->b[2] = p[1]; out->b[3] = p[0];
    out->b[4] = p[5]; out->b[5] = p[4];
    out->b[6] = p[7]; out->b[7] = p[6];
    memcpy(&out->b[8], &p[8], 8);
}

/* ---------- Tabelle ---------- */

static int cmp_state(const void *a, const void *b)
{
    return memcmp(((const lox_state_ref_t *)a)->uuid.b, ((const lox_state_ref_t *)b)->uuid.b, 16);
}

static void add_state(const char *uuid_str, const char *ctrl, const char *type,
                      const char *state, const char *room)
{
    lox_uuid_t u;
    if (!lox_uuid_parse(uuid_str, &u)) return;
    if (s_state_count == s_state_cap) {
        size_t cap = s_state_cap ? s_state_cap * 2 : 1024;
        lox_state_ref_t *n = heap_caps_realloc(s_states, cap * sizeof(*n), MALLOC_CAP_SPIRAM);
        if (n == NULL) return;
        s_states = n;
        s_state_cap = cap;
    }
    s_states[s_state_count++] = (lox_state_ref_t){
        .uuid = u, .control_name = ctrl, .control_type = type, .state_name = state, .room_name = room,
    };
}

static const char *room_name_of(const cJSON *ctrl, const char *fallback)
{
    const cJSON *room = cJSON_GetObjectItem(ctrl, "room");
    const cJSON *rooms = cJSON_GetObjectItem(s_root, "rooms");
    if (cJSON_IsString(room) && rooms) {
        const cJSON *r = cJSON_GetObjectItem(rooms, room->valuestring);
        const cJSON *n = r ? cJSON_GetObjectItem(r, "name") : NULL;
        if (cJSON_IsString(n)) return n->valuestring;
    }
    return fallback;
}

const char *lox_struct_room_of(const cJSON *ctrl)
{
    return room_name_of(ctrl, NULL);
}

static const cJSON *find_ctrl(const cJSON *ctrls, const char *uuid, const char *parent_room, const char **room)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, ctrls) {
        const char *r = room_name_of(c, parent_room);
        const cJSON *u = cJSON_GetObjectItem(c, "uuidAction");
        if (cJSON_IsString(u) && !strcmp(u->valuestring, uuid)) {
            if (room) *room = r;
            return c;
        }
        const cJSON *hit = find_ctrl(cJSON_GetObjectItem(c, "subControls"), uuid, r, room);
        if (hit) return hit;
    }
    return NULL;
}

const cJSON *lox_struct_find_control(const char *uuid_action, const char **room)
{
    if (room) *room = NULL;
    if (s_root == NULL || uuid_action == NULL) return NULL;
    return find_ctrl(cJSON_GetObjectItem(s_root, "controls"), uuid_action, NULL, room);
}

static void add_control(const cJSON *ctrl, const char *parent_room)
{
    const cJSON *name = cJSON_GetObjectItem(ctrl, "name");
    const cJSON *type = cJSON_GetObjectItem(ctrl, "type");
    const char *cname = cJSON_IsString(name) ? name->valuestring : "?";
    const char *ctype = cJSON_IsString(type) ? type->valuestring : "?";
    const char *room = room_name_of(ctrl, parent_room);
    s_info.controls++;

    const cJSON *states = cJSON_GetObjectItem(ctrl, "states");
    const cJSON *st;
    cJSON_ArrayForEach(st, states) {
        if (cJSON_IsString(st)) {
            add_state(st->valuestring, cname, ctype, st->string, room);
        } else if (cJSON_IsArray(st)) {
            const cJSON *e;
            cJSON_ArrayForEach(e, st) {
                if (cJSON_IsString(e)) add_state(e->valuestring, cname, ctype, st->string, room);
            }
        }
    }

    const cJSON *subs = cJSON_GetObjectItem(ctrl, "subControls");
    const cJSON *sc;
    cJSON_ArrayForEach(sc, subs) {
        add_control(sc, room);
    }
}

esp_err_t lox_struct_load(char *json, size_t len)
{
    lox_struct_lock(portMAX_DELAY);
    lox_struct_free();
    s_generation++;

    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    s_root = cJSON_ParseWithLength(json, len);
    heap_caps_free(json);
    if (s_root == NULL) {
        ESP_LOGE(TAG, "LoxAPP3.json nicht lesbar");
        lox_struct_unlock();
        return ESP_FAIL;
    }

    const cJSON *ms = cJSON_GetObjectItem(s_root, "msInfo");
    const cJSON *msn = ms ? cJSON_GetObjectItem(ms, "msName") : NULL;
    if (cJSON_IsString(msn)) strlcpy(s_info.ms_name, msn->valuestring, sizeof(s_info.ms_name));
    const cJSON *lm = cJSON_GetObjectItem(s_root, "lastModified");
    if (cJSON_IsString(lm)) strlcpy(s_info.last_modified, lm->valuestring, sizeof(s_info.last_modified));

    s_info.rooms = cJSON_GetArraySize(cJSON_GetObjectItem(s_root, "rooms"));
    s_info.categories = cJSON_GetArraySize(cJSON_GetObjectItem(s_root, "cats"));

    const cJSON *controls = cJSON_GetObjectItem(s_root, "controls");
    const cJSON *c;
    cJSON_ArrayForEach(c, controls) {
        add_control(c, NULL);
    }

    /* Globale Status (z. B. Betriebsmodus, Sonnenauf-/untergang) */
    const cJSON *gstates = cJSON_GetObjectItem(s_root, "globalStates");
    const cJSON *g;
    cJSON_ArrayForEach(g, gstates) {
        if (cJSON_IsString(g)) add_state(g->valuestring, "Global", "global", g->string, NULL);
    }

    qsort(s_states, s_state_count, sizeof(*s_states), cmp_state);
    s_info.states = s_state_count;
    s_values = heap_caps_calloc(s_state_count ? s_state_count : 1, sizeof(double), MALLOC_CAP_SPIRAM);
    s_texts = heap_caps_calloc(s_state_count ? s_state_count : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    s_dts = heap_caps_calloc(s_state_count ? s_state_count : 1, sizeof(dt_blob_t *), MALLOC_CAP_SPIRAM);
    lox_struct_unlock();

    ESP_LOGI(TAG, "Struktur \"%s\" (Stand %s): %u Räume, %u Kategorien, %u Bausteine, %u Status",
             s_info.ms_name, s_info.last_modified, (unsigned)s_info.rooms, (unsigned)s_info.categories,
             (unsigned)s_info.controls, (unsigned)s_info.states);
    return ESP_OK;
}

void lox_struct_free(void)
{
    lox_struct_lock(portMAX_DELAY);
    if (s_root) cJSON_Delete(s_root);
    s_root = NULL;
    portENTER_CRITICAL(&s_vlock);
    double *values = s_values;
    char **texts = s_texts;
    dt_blob_t **dts = s_dts;
    size_t count = s_state_count;
    s_values = NULL;
    s_texts = NULL;
    s_dts = NULL;
    portEXIT_CRITICAL(&s_vlock);
    if (texts) {
        for (size_t i = 0; i < count; i++) free(texts[i]);
    }
    if (dts) {
        for (size_t i = 0; i < count; i++) free(dts[i]);
    }
    heap_caps_free(dts);
    heap_caps_free(texts);
    heap_caps_free(values);
    heap_caps_free(s_states);
    s_states = NULL;
    s_state_count = s_state_cap = 0;
    memset(&s_info, 0, sizeof(s_info));
    lox_struct_unlock();
}

/* Index in der sortierten Tabelle oder -1 */
static int state_index(const lox_uuid_t *uuid)
{
    const lox_state_ref_t *r = lox_struct_find_state(uuid);
    return r ? (int)(r - s_states) : -1;
}

void lox_value_set(const lox_uuid_t *uuid, double v)
{
    int i = state_index(uuid);
    if (i < 0) return;
    portENTER_CRITICAL(&s_vlock);
    if (s_values) {
        s_values[i] = v;
        s_changes++;
    }
    portEXIT_CRITICAL(&s_vlock);
}

void lox_text_set(const lox_uuid_t *uuid, const char *text)
{
    int i = state_index(uuid);
    if (i < 0) return;
    size_t len = strlen(text);
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (copy == NULL) return;
    memcpy(copy, text, len + 1);
    char *old = NULL;
    portENTER_CRITICAL(&s_vlock);
    if (s_texts) {
        old = s_texts[i];
        s_texts[i] = copy;
        copy = NULL;
        s_changes++;
    }
    portEXIT_CRITICAL(&s_vlock);
    free(old);
    free(copy);
}

void lox_daytimer_set(const lox_uuid_t *uuid, double def_value, const lox_dt_entry_t *e, int n)
{
    int i = state_index(uuid);
    if (i < 0 || n < 0) return;
    dt_blob_t *b = heap_caps_malloc(sizeof(dt_blob_t) + n * sizeof(lox_dt_entry_t), MALLOC_CAP_SPIRAM);
    if (b == NULL) return;
    b->def = def_value;
    b->n = n;
    memcpy(b->e, e, n * sizeof(lox_dt_entry_t));
    dt_blob_t *old = NULL;
    portENTER_CRITICAL(&s_vlock);
    if (s_dts) {
        old = s_dts[i];
        s_dts[i] = b;
        b = NULL;
        s_changes++;
    }
    portEXIT_CRITICAL(&s_vlock);
    free(old);
    free(b);
}

int lox_daytimer_get(const lox_uuid_t *uuid, double *def_value, lox_dt_entry_t *out, int max)
{
    if (!lox_struct_lock(0)) return -1;
    int i = state_index(uuid);
    int n = -1;
    if (i >= 0) {
        portENTER_CRITICAL(&s_vlock);
        const dt_blob_t *b = s_dts ? s_dts[i] : NULL;
        if (b) {
            n = b->n < max ? b->n : max;
            if (def_value) *def_value = b->def;
            memcpy(out, b->e, n * sizeof(lox_dt_entry_t));
        }
        portEXIT_CRITICAL(&s_vlock);
    }
    lox_struct_unlock();
    return n;
}

bool lox_value_get(const lox_uuid_t *uuid, double *out)
{
    if (!lox_struct_lock(0)) return false;      // Struktur wird gerade neu geladen
    int i = state_index(uuid);
    bool ok = false;
    if (i >= 0) {
        portENTER_CRITICAL(&s_vlock);
        if (s_values) {
            *out = s_values[i];
            ok = true;
        }
        portEXIT_CRITICAL(&s_vlock);
    }
    lox_struct_unlock();
    return ok;
}

bool lox_value_get_str(const char *uuid, double *out)
{
    lox_uuid_t u;
    return lox_uuid_parse(uuid, &u) && lox_value_get(&u, out);
}

bool lox_text_get(const lox_uuid_t *uuid, char *out, size_t size)
{
    if (!lox_struct_lock(0)) return false;
    int i = state_index(uuid);
    bool ok = false;
    if (i >= 0) {
        portENTER_CRITICAL(&s_vlock);
        if (s_texts && s_texts[i]) {
            strlcpy(out, s_texts[i], size);
            ok = true;
        }
        portEXIT_CRITICAL(&s_vlock);
    }
    lox_struct_unlock();
    return ok;
}

bool lox_struct_loaded(void)
{
    return s_root != NULL;
}

void lox_struct_get_info(lox_struct_info_t *out)
{
    *out = s_info;
}

const lox_state_ref_t *lox_struct_find_state(const lox_uuid_t *uuid)
{
    if (s_states == NULL) return NULL;
    lox_state_ref_t key = { .uuid = *uuid };
    return bsearch(&key, s_states, s_state_count, sizeof(*s_states), cmp_state);
}

const cJSON *lox_struct_root(void)
{
    return s_root;
}

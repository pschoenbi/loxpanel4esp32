/*
 * Kamerabild: HTTP (MJPEG oder einzelne JPEGs) -> Hardware-JPEG-Decoder -> PPA-Skalierung -> RGB565.
 *
 * Bilder werden im Datenstrom an den JPEG-Markern erkannt (FFD8 ... FFD9). Das funktioniert für
 * multipart/x-mixed-replace (MJPEG) ebenso wie für einzelne JPEG-Antworten (dann wird neu abgefragt).
 * Drei Ausgabepuffer: einer wird angezeigt, einer ist fertig, in den dritten wird geschrieben.
 */

#include "cam.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cam";

#define NBUF            3
#define OUT_MAX_W       1024
#define OUT_MAX_H       600
#define ACC_CAP         (1536 * 1024)   // Empfangspuffer (ein JPEG muss hineinpassen)
#define READ_CHUNK      16384
#define ALIGN_UP(x, a)  (((x) + (a) - 1) / (a) * (a))

/* Konfiguration (unter s_mx) */
static SemaphoreHandle_t s_mx;
static TaskHandle_t s_task;
static volatile bool s_active;
static volatile uint32_t s_session;
static char s_url[384], s_user[64], s_pass[96];
static int s_max_w = OUT_MAX_W, s_max_h = OUT_MAX_H;
static char s_status[64] = "aus";

/* Ausgabe (unter s_mx) */
static uint8_t *s_out[NBUF];
static size_t s_out_size;
static int s_out_w[NBUF], s_out_h[NBUF];
static int s_ready = -1, s_shown = -1;
static uint32_t s_seq;

/* nur im Kamera-Task */
static jpeg_decoder_handle_t s_dec;
static ppa_client_handle_t s_ppa;
static uint8_t *s_acc;
static uint8_t *s_dec_in, *s_dec_out;
static size_t s_dec_in_size, s_dec_out_size;

static void set_status(const char *fmt, int v)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(s_status, sizeof(s_status), fmt, v);
    xSemaphoreGive(s_mx);
}

static bool still_wanted(uint32_t sess)
{
    return s_active && s_session == sess;
}

/* ---------- Dekodieren + Skalieren ---------- */

static bool ensure_dec_buf(uint8_t **buf, size_t *size, size_t need, jpeg_dec_buffer_alloc_direction_t dir)
{
    if (*buf && *size >= need) return true;
    free(*buf);
    *buf = NULL;
    *size = 0;
    jpeg_decode_memory_alloc_cfg_t mc = { .buffer_direction = dir };
    size_t got = 0;
    *buf = jpeg_alloc_decoder_mem(need, &mc, &got);
    if (*buf == NULL) return false;
    *size = got;
    return true;
}

static void process_frame(const uint8_t *data, size_t len)
{
    static uint32_t s_log_at, s_frames, s_bytes, s_fail;
    static int64_t s_t0;
    if (s_t0 == 0) s_t0 = esp_timer_get_time();

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(data, len, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        s_fail++;
        return;
    }
    int w = info.width, h = info.height, pw, ph;
    switch (info.sample_method) {
    case JPEG_DOWN_SAMPLING_YUV420: pw = ALIGN_UP(w, 16); ph = ALIGN_UP(h, 16); break;
    case JPEG_DOWN_SAMPLING_YUV422: pw = ALIGN_UP(w, 16); ph = ALIGN_UP(h, 8); break;
    default:                        pw = ALIGN_UP(w, 8);  ph = ALIGN_UP(h, 8); break;
    }
    if (!ensure_dec_buf(&s_dec_in, &s_dec_in_size, ALIGN_UP(len, 4096), JPEG_DEC_ALLOC_INPUT_BUFFER) ||
        !ensure_dec_buf(&s_dec_out, &s_dec_out_size, (size_t)pw * ph * 2, JPEG_DEC_ALLOC_OUTPUT_BUFFER)) {
        set_status("Kein Speicher für %d px", w);
        return;
    }
    memcpy(s_dec_in, data, len);
    jpeg_decode_cfg_t dc = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    if (jpeg_decoder_process(s_dec, &dc, s_dec_in, len, s_dec_out, s_dec_out_size, &out_size) != ESP_OK) {
        s_fail++;
        return;
    }

    /* Auf die Zielgrösse einpassen (PPA skaliert in 1/16-Schritten) */
    int max_w, max_h;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    max_w = s_max_w;
    max_h = s_max_h;
    xSemaphoreGive(s_mx);
    float s = fminf((float)max_w / w, (float)max_h / h);
    if (s > 1.0f) s = 1.0f;
    s = floorf(s * 16.0f) / 16.0f;
    if (s < 1.0f / 16) s = 1.0f / 16;
    int ow = (int)(w * s), oh = (int)(h * s);
    if (ow < 1 || oh < 1) return;

    /* freien Ausgabepuffer wählen */
    int idx = -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NBUF; i++) {
        if (i != s_shown && i != s_ready) {
            idx = i;
            break;
        }
    }
    xSemaphoreGive(s_mx);
    if (idx < 0) return;

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = s_dec_out, .pic_w = pw, .pic_h = ph, .block_w = w, .block_h = h,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_out[idx], .buffer_size = s_out_size, .pic_w = ow, .pic_h = oh,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = s,
        .scale_y = s,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Skalierung %dx%d -> %dx%d: %s", w, h, ow, oh, esp_err_to_name(err));
        s_fail++;
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_out_w[idx] = ow;
    s_out_h[idx] = oh;
    s_ready = idx;
    s_seq++;
    xSemaphoreGive(s_mx);

    s_frames++;
    s_bytes += len;
    int64_t now = esp_timer_get_time();
    if (s_log_at == 0 || now - s_t0 > 10000000) {
        float secs = (now - s_t0) / 1e6f;
        ESP_LOGI(TAG, "Bild %dx%d (%s) -> %dx%d, %.1f B/s, %u kB/Bild, %u Fehler",
                 w, h, info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? "4:2:0" :
                 info.sample_method == JPEG_DOWN_SAMPLING_YUV422 ? "4:2:2" : "4:4:4/grau",
                 ow, oh, secs > 0 ? s_frames / secs : 0, (unsigned)(s_bytes / (s_frames ? s_frames : 1) / 1024),
                 (unsigned)s_fail);
        s_log_at = 1;
        s_t0 = now;
        s_frames = s_bytes = s_fail = 0;
    }
}

/* ---------- Empfang ---------- */

static int find_marker(const uint8_t *b, size_t from, size_t len, uint8_t second)
{
    for (size_t i = from; i + 1 < len; i++) {
        if (b[i] == 0xFF && b[i + 1] == second) return (int)i;
    }
    return -1;
}

/* ---------- go2rtc: Tonformate der Quelle ermitteln (nur Diagnose) ---------- */

/* Fragt /api/streams?src=... ab und loggt nur die Medienbeschreibungen (keine URLs/Zugangsdaten). */
static void probe_media(const char *url, const char *user, const char *pass)
{
    static char s_probed[384];
    if (!strcmp(s_probed, url)) return;
    strlcpy(s_probed, url, sizeof(s_probed));

    const char *api = strstr(url, "/api/");
    const char *q = strchr(url, '?');
    if (api == NULL || q == NULL) return;
    char purl[384];
    snprintf(purl, sizeof(purl), "%.*s/api/streams%s", (int)(api - url), url, q);
    esp_http_client_config_t cfg = {
        .url = purl,
        .username = user[0] ? user : NULL,
        .password = user[0] ? pass : NULL,
        .auth_type = user[0] ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return;
    char *body = heap_caps_calloc(1, 8192, MALLOC_CAP_SPIRAM);
    if (body && esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int code = esp_http_client_get_status_code(c);
        int n = 0, r;
        while (n < 8191 && (r = esp_http_client_read(c, body + n, 8191 - n)) > 0) n += r;
        ESP_LOGI(TAG, "go2rtc-Quelle: HTTP %d, %d Bytes", code, n);
        cJSON *j = cJSON_Parse(body);
        const cJSON *prod;
        cJSON_ArrayForEach(prod, cJSON_GetObjectItem(j, "producers")) {
            const cJSON *m;
            cJSON_ArrayForEach(m, cJSON_GetObjectItem(prod, "medias")) {
                if (cJSON_IsString(m)) ESP_LOGI(TAG, "  Medium: %s", m->valuestring);
            }
            const cJSON *recv;
            cJSON_ArrayForEach(recv, cJSON_GetObjectItem(prod, "receivers")) {
                const cJSON *codec = cJSON_GetObjectItem(recv, "codec");
                const cJSON *cn = cJSON_GetObjectItem(codec, "codec_name");
                const cJSON *cr = cJSON_GetObjectItem(codec, "sample_rate");
                if (cJSON_IsString(cn)) ESP_LOGI(TAG, "  Empfänger: %s %d Hz", cn->valuestring,
                                                  cJSON_IsNumber(cr) ? cr->valueint : 0);
            }
            const cJSON *src = cJSON_GetObjectItem(prod, "format_name");
            if (cJSON_IsString(src)) ESP_LOGI(TAG, "  Quelle: %s", src->valuestring);
        }
        if (j == NULL) ESP_LOGI(TAG, "  Antwort ist kein JSON");
        cJSON_Delete(j);
    }
    heap_caps_free(body);
    esp_http_client_cleanup(c);
}

static void run_stream(uint32_t sess)
{
    char url[384], user[64], pass[96];
    xSemaphoreTake(s_mx, portMAX_DELAY);
    strlcpy(url, s_url, sizeof(url));
    strlcpy(user, s_user, sizeof(user));
    strlcpy(pass, s_pass, sizeof(pass));
    xSemaphoreGive(s_mx);

    set_status("Verbinde ...%.0d", 0);
    esp_http_client_config_t cfg = {
        .url = url,
        .username = user[0] ? user : NULL,
        .password = user[0] ? pass : NULL,
        .auth_type = user[0] ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
        .timeout_ms = 5000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        set_status("HTTP-Fehler%.0d", 0);
        return;
    }
    if (esp_http_client_open(c, 0) != ESP_OK) {
        set_status("Kamera nicht erreichbar%.0d", 0);
        esp_http_client_cleanup(c);
        return;
    }
    esp_http_client_fetch_headers(c);
    int code = esp_http_client_get_status_code(c);
    char *ct = NULL;
    esp_http_client_get_header(c, "Content-Type", &ct);
    static bool logged;
    if (!logged || code != 200) {
        ESP_LOGI(TAG, "Stream: HTTP %d, %s", code, ct ? ct : "-");
        logged = code == 200;
    }
    if (code == 401 || code == 403) {
        set_status("Zugang abgelehnt (%d)", code);
        esp_http_client_cleanup(c);
        return;
    }
    if (code != 200) {
        set_status("Kamera meldet Fehler %d", code);
        esp_http_client_cleanup(c);
        return;
    }
    set_status("Empfange ...%.0d", 0);

    size_t len = 0, scan = 0;
    int start = -1;
    int frames = 0;
    while (still_wanted(sess)) {
        if (len + READ_CHUNK > ACC_CAP) {
            /* kein vollständiges Bild im Puffer: verwerfen */
            len = 0;
            scan = 0;
            start = -1;
        }
        int n = esp_http_client_read(c, (char *)s_acc + len, READ_CHUNK);
        if (n < 0) break;
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(c)) break;     // Einzelbild: neu abfragen
            continue;
        }
        len += n;
        for (;;) {
            if (start < 0) {
                start = find_marker(s_acc, scan, len, 0xD8);
                if (start < 0) {
                    /* nur das letzte Byte behalten (könnte 0xFF sein) */
                    if (len > 0) {
                        s_acc[0] = s_acc[len - 1];
                        len = 1;
                    }
                    scan = 0;
                    break;
                }
                scan = start + 2;
            }
            int end = find_marker(s_acc, scan, len, 0xD9);
            if (end < 0) {
                scan = len > 1 ? len - 1 : 0;
                break;
            }
            process_frame(s_acc + start, end + 2 - start);
            /* go2rtc kennt die Tonspur erst bei laufendem Stream: nach einigen Bildern abfragen */
            if (++frames == 20) probe_media(url, user, pass);
            size_t rest = len - (end + 2);
            memmove(s_acc, s_acc + end + 2, rest);
            len = rest;
            scan = 0;
            start = -1;
            if (!still_wanted(sess)) break;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

static void cam_task(void *arg)
{
    for (;;) {
        if (!s_active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        uint32_t sess = s_session;
        int64_t t0 = esp_timer_get_time();
        run_stream(sess);
        if (!still_wanted(sess)) {
            set_status("aus%.0d", 0);
            continue;
        }
        /* Einzelbilder: gleich weiter; nach Fehlern kurz warten */
        int64_t dt = esp_timer_get_time() - t0;
        vTaskDelay(pdMS_TO_TICKS(dt < 1000000 ? 1000 : 100));
    }
}

static esp_err_t init_once(void)
{
    if (s_task) return ESP_OK;
    if (s_mx == NULL) s_mx = xSemaphoreCreateMutex();
    if (s_mx == NULL) return ESP_ERR_NO_MEM;

    s_acc = heap_caps_malloc(ACC_CAP, MALLOC_CAP_SPIRAM);
    s_out_size = ALIGN_UP(OUT_MAX_W * OUT_MAX_H * 2, 128);
    for (int i = 0; i < NBUF; i++) {
        s_out[i] = heap_caps_aligned_calloc(128, 1, s_out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (s_out[i] == NULL) s_out[i] = heap_caps_aligned_calloc(128, 1, s_out_size, MALLOC_CAP_SPIRAM);
    }
    if (s_acc == NULL || !s_out[0] || !s_out[1] || !s_out[2]) return ESP_ERR_NO_MEM;

    jpeg_decode_engine_cfg_t ec = { .intr_priority = 0, .timeout_ms = 500 };
    esp_err_t err = jpeg_new_decoder_engine(&ec, &s_dec);
    if (err != ESP_OK) return err;
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
    err = ppa_register_client(&pc, &s_ppa);
    if (err != ESP_OK) return err;

    if (xTaskCreate(cam_task, "cam", 6144, NULL, 4, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

/* ---------- öffentlich ---------- */

esp_err_t cam_start(const char *url, const char *user, const char *pass, int max_w, int max_h)
{
    esp_err_t err = init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initialisierung: %s", esp_err_to_name(err));
        return err;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    strlcpy(s_url, url, sizeof(s_url));
    strlcpy(s_user, user ? user : "", sizeof(s_user));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    s_max_w = max_w > OUT_MAX_W ? OUT_MAX_W : max_w;
    s_max_h = max_h > OUT_MAX_H ? OUT_MAX_H : max_h;
    s_ready = -1;
    s_session++;
    s_active = true;
    snprintf(s_status, sizeof(s_status), "Verbinde ...");
    xSemaphoreGive(s_mx);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void cam_stop(void)
{
    if (s_mx == NULL) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_active = false;
    s_session++;
    s_ready = -1;
    s_shown = -1;
    xSemaphoreGive(s_mx);
}

bool cam_get_frame(cam_frame_t *out)
{
    if (s_mx == NULL) return false;
    bool ok = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_ready >= 0) {
        s_shown = s_ready;
        s_ready = -1;
        out->pixels = s_out[s_shown];
        out->w = s_out_w[s_shown];
        out->h = s_out_h[s_shown];
        out->seq = s_seq;
        ok = true;
    }
    xSemaphoreGive(s_mx);
    return ok;
}

void cam_status(char *out, size_t size)
{
    if (s_mx == NULL) {
        strlcpy(out, "aus", size);
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    strlcpy(out, s_status, size);
    xSemaphoreGive(s_mx);
}

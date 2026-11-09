/*
 * Geräte-Einstellungen im NVS.
 */

#include "dev_settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
#define NS "dev"

static dev_settings_t s_set;
static SemaphoreHandle_t s_mutex;

typedef struct {
    const char *key;
    size_t off;
    size_t size;
} field_t;

#define F(k, m) { k, offsetof(dev_settings_t, m), sizeof(((dev_settings_t *)0)->m) }
static const field_t FIELDS[] = {
    F("w_ssid", wifi_ssid), F("w_pass", wifi_pass), F("ip", ip), F("mask", mask), F("gw", gw),
    F("mac", mac), F("ms_host", ms_host), F("ms_user", ms_user), F("ms_pass", ms_pass),
    F("web_hash", web_hash),
};
#define NFIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

static void hash_pw(const char *pw, char out[65])
{
    uint8_t h[32];
    size_t hl = 0;
    char salted[160];
    snprintf(salted, sizeof(salted), "loxpanel:%s", pw);
    psa_crypto_init();
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)salted, strlen(salted), h, sizeof(h), &hl);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
    out[64] = '\0';
}

static esp_err_t write_all(const dev_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < NFIELDS; i++) {
        const char *v = (const char *)s + FIELDS[i].off;
        err = nvs_set_str(h, FIELDS[i].key, v);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_set_u8(h, "seeded", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_set, 0, sizeof(s_set));

    nvs_handle_t h;
    uint8_t seeded = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "seeded", &seeded);
        for (size_t i = 0; i < NFIELDS; i++) {
            size_t len = FIELDS[i].size;
            nvs_get_str(h, FIELDS[i].key, (char *)&s_set + FIELDS[i].off, &len);
        }
        nvs_close(h);
    }

    if (!seeded) {
        /* Erster Start: Werte aus sdkconfig übernehmen (Entwicklung) */
        strlcpy(s_set.wifi_ssid, CONFIG_LOXPANEL_WIFI_SSID, sizeof(s_set.wifi_ssid));
        strlcpy(s_set.wifi_pass, CONFIG_LOXPANEL_WIFI_PASSWORD, sizeof(s_set.wifi_pass));
        strlcpy(s_set.ip, CONFIG_LOXPANEL_WIFI_STATIC_IP, sizeof(s_set.ip));
        strlcpy(s_set.mask, CONFIG_LOXPANEL_WIFI_STATIC_MASK, sizeof(s_set.mask));
        strlcpy(s_set.gw, CONFIG_LOXPANEL_WIFI_STATIC_GW, sizeof(s_set.gw));
        strlcpy(s_set.mac, CONFIG_LOXPANEL_WIFI_MAC, sizeof(s_set.mac));
        strlcpy(s_set.ms_host, CONFIG_LOXPANEL_MS_HOST, sizeof(s_set.ms_host));
        strlcpy(s_set.ms_user, CONFIG_LOXPANEL_MS_USER, sizeof(s_set.ms_user));
        strlcpy(s_set.ms_pass, CONFIG_LOXPANEL_MS_PASSWORD, sizeof(s_set.ms_pass));
        esp_err_t err = write_all(&s_set);
        ESP_LOGI(TAG, "Einstellungen aus sdkconfig übernommen: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "WLAN \"%s\", %s%s, Miniserver %s, Web-Passwort %s",
             s_set.wifi_ssid, s_set.ip[0] ? "feste IP " : "DHCP", s_set.ip,
             s_set.ms_host[0] ? s_set.ms_host : "-", s_set.web_hash[0] ? "gesetzt" : "fehlt");
    return ESP_OK;
}

void settings_get(dev_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_set;
    xSemaphoreGive(s_mutex);
}

esp_err_t settings_save(const dev_settings_t *s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_set = *s;
    esp_err_t err = write_all(&s_set);
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Einstellungen gespeichert: %s", esp_err_to_name(err));
    return err;
}

bool settings_has_wifi(void)
{
    return s_set.wifi_ssid[0] != '\0';
}

esp_err_t settings_set_web_password(const char *pw)
{
    if (pw == NULL || strlen(pw) < 4) return ESP_ERR_INVALID_ARG;
    dev_settings_t s;
    settings_get(&s);
    hash_pw(pw, s.web_hash);
    return settings_save(&s);
}

bool settings_check_web_password(const char *pw)
{
    if (!s_set.web_hash[0] || pw == NULL) return false;
    char h[65];
    hash_pw(pw, h);
    return strcmp(h, s_set.web_hash) == 0;
}

bool settings_has_web_password(void)
{
    return s_set.web_hash[0] != '\0';
}

void settings_request_setup(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "setup_req", 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool settings_take_setup_request(void)
{
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u8(h, "setup_req", &v);
        if (v) {
            nvs_erase_key(h, "setup_req");
            nvs_commit(h);
        }
        nvs_close(h);
    }
    return v != 0;
}

void settings_forget_ms_password(void)
{
    if (!s_set.ms_pass[0]) return;
    dev_settings_t s;
    settings_get(&s);
    s.ms_pass[0] = '\0';
    settings_save(&s);
    ESP_LOGI(TAG, "Miniserver-Passwort gelöscht (Token vorhanden)");
}

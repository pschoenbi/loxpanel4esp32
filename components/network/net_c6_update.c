/*
 * ESP32-C6-Update über SDIO (esp_hosted Slave-OTA).
 * Ablauf nach Espressif/Elecrow-Beispiel "host_performs_slave_ota":
 *   connect -> Version prüfen -> ota_begin -> ota_write (Blöcke) -> ota_end
 *   -> ota_activate (nur wenn alter Slave >= 2.6) -> Host-Neustart
 * Die neue Firmware landet in der inaktiven OTA-Partition des C6. Bricht etwas ab,
 * bootet der C6 weiter die alte Version.
 */

#include "net_c6_update.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"

static const char *TAG = "net_c6_update";

#define CHUNK_SIZE          1500
#define C6_CHIP_ID          13          // ESP_CHIP_ID_ESP32C6

extern const uint8_t c6_fw_start[] asm("_binary_network_adapter_2_12_3_bin_start");
extern const uint8_t c6_fw_end[]   asm("_binary_network_adapter_2_12_3_bin_end");

static void report(net_c6_progress_cb_t cb, const char *text, int percent)
{
    ESP_LOGI(TAG, "%s", text);
    if (cb) {
        cb(text, percent);
    }
}

/* Eingebettetes Image prüfen: Magic, Chip, App-Beschreibung. Liefert die Version. */
static bool image_ok(const uint8_t *img, size_t len, char *ver, size_t ver_len)
{
    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (len < desc_off + sizeof(esp_app_desc_t)) {
        return false;
    }
    const esp_image_header_t *hdr = (const esp_image_header_t *)img;
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || hdr->chip_id != C6_CHIP_ID) {
        ESP_LOGE(TAG, "Image ist keine ESP32-C6-App (magic 0x%02x, chip %u)", hdr->magic, hdr->chip_id);
        return false;
    }
    esp_app_desc_t desc;
    memcpy(&desc, img + desc_off, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD || strcmp(desc.project_name, "network_adapter") != 0) {
        ESP_LOGE(TAG, "App-Beschreibung ungültig");
        return false;
    }
    strlcpy(ver, desc.version, ver_len);
    return true;
}

net_c6_update_result_t net_c6_update_run(net_c6_progress_cb_t cb)
{
    char msg[96];
    const uint8_t *img = c6_fw_start;
    const size_t len = c6_fw_end - c6_fw_start;

    char img_ver[32];
    if (!image_ok(img, len, img_ver, sizeof(img_ver))) {
        report(cb, "C6-Image ungueltig, kein Update", -1);
        return NET_C6_UPDATE_FAILED;
    }

    report(cb, "Verbinde mit ESP32-C6 ...", -1);
    if (esp_hosted_connect_to_slave() != ESP_OK) {
        report(cb, "Keine Verbindung zum C6", -1);
        return NET_C6_UPDATE_FAILED;
    }

    esp_hosted_coprocessor_fwver_t cur = { 0 };
    if (esp_hosted_get_coprocessor_fwversion(&cur) != ESP_OK) {
        report(cb, "C6-Version nicht lesbar, kein Update", -1);
        return NET_C6_UPDATE_FAILED;
    }
    char cur_ver[32];
    snprintf(cur_ver, sizeof(cur_ver), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, cur.major1, cur.minor1, cur.patch1);
    ESP_LOGI(TAG, "C6 aktuell %s, Image %s (%u Bytes)", cur_ver, img_ver, (unsigned)len);

    if (strcmp(cur_ver, img_ver) == 0) {
        snprintf(msg, sizeof(msg), "C6 hat bereits %s - kein Update noetig", cur_ver);
        report(cb, msg, 100);
        return NET_C6_UPDATE_NOT_NEEDED;
    }

    snprintf(msg, sizeof(msg), "C6-Update %s -> %s startet", cur_ver, img_ver);
    report(cb, msg, 0);

    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        snprintf(msg, sizeof(msg), "OTA-Start abgelehnt: %s", esp_err_to_name(err));
        report(cb, msg, -1);
        return NET_C6_UPDATE_FAILED;
    }

    static uint8_t chunk[CHUNK_SIZE];
    size_t off = 0;
    int last_pct = -1;
    while (off < len) {
        size_t n = (len - off) > CHUNK_SIZE ? CHUNK_SIZE : (len - off);
        memcpy(chunk, img + off, n);
        err = esp_hosted_slave_ota_write(chunk, n);
        if (err != ESP_OK) {
            snprintf(msg, sizeof(msg), "Uebertragung abgebrochen bei %u kB: %s",
                     (unsigned)(off / 1024), esp_err_to_name(err));
            report(cb, msg, -1);
            esp_hosted_slave_ota_end();
            return NET_C6_UPDATE_FAILED;
        }
        off += n;
        int pct = (int)(off * 100 / len);
        if (pct != last_pct && (pct % 5 == 0 || off == len)) {
            last_pct = pct;
            snprintf(msg, sizeof(msg), "C6-Update: %d %% (%u / %u kB)", pct,
                     (unsigned)(off / 1024), (unsigned)(len / 1024));
            report(cb, msg, pct);
        }
    }

    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        snprintf(msg, sizeof(msg), "C6 hat das Image nicht akzeptiert: %s", esp_err_to_name(err));
        report(cb, msg, -1);
        return NET_C6_UPDATE_FAILED;
    }

    /* Aktivieren gibt es erst ab Slave 2.6; ältere schalten bei ota_end selbst um. */
    if (cur.major1 > 2 || (cur.major1 == 2 && cur.minor1 > 5)) {
        err = esp_hosted_slave_ota_activate();
        if (err != ESP_OK) {
            snprintf(msg, sizeof(msg), "Aktivieren fehlgeschlagen: %s", esp_err_to_name(err));
            report(cb, msg, -1);
            return NET_C6_UPDATE_FAILED;
        }
    }

    report(cb, "C6-Update fertig - Neustart", 100);
    return NET_C6_UPDATE_DONE;
}

/*
 * LVGL-Anbindung, für alle MIPI-DSI-Boards gleich.
 */

#include "board.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board_lvgl";

lv_display_t *board_lvgl_start(void)
{
    const board_info_t *info = board_info();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* TrueType-Rasterizer (TinyTTF) braucht für grosse Schriften viel Stack;
       mit dem Standard (7 KB) läuft er bei 120-px-Ziffern über. */
    port_cfg.task_stack = 24 * 1024;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init fehlgeschlagen");
        return NULL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = board_panel_io(),
        .panel_handle = board_panel(),
        .buffer_size = info->width * 40,   // zwei Teilpuffer à 40 Zeilen im internen RAM
        .double_buffer = true,
        .hres = info->width,
        .vres = info->height,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = { .avoid_tearing = false },
    };

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "Display konnte nicht bei LVGL angemeldet werden");
        return NULL;
    }

    if (board_touch() != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = board_touch(),
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "Touch konnte nicht bei LVGL angemeldet werden");
        }
    }

    return disp;
}

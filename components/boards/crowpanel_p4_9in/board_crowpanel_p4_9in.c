/*
 * Hardware-Profil: Elecrow CrowPanel Advanced ESP32-P4 9" (1024x600)
 *
 * Quellen (Elecrow-RD/CrowPanel-Advanced-9inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen):
 *   - Eagle_SCH&PCB/1.2/readme.md  (Hardware-Handbuch, Pins und Timings aus dem IDF-Beispiel Lesson07)
 *   - example/V1.0/Arduino_Code/Lesson05-Touchscreen  (LDO3/LDO4, GT911-Pins)
 *   - Elecrow-Wiki "P4 IDF 07 Turn on the Screen"
 * Laut Elecrow ist die Hardware von 7", 9" und 10.1" identisch.
 */

#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

static const char *TAG = "board_9in";

/* --- Auflösung --- */
#define LCD_H_RES               1024
#define LCD_V_RES               600

/* --- Stromversorgung (interne LDOs des P4) --- */
#define LDO_MIPI_PHY_CHAN       3       // 2,5 V für MIPI-DSI-PHY
#define LDO_MIPI_PHY_MV         2500
#define LDO_PANEL_CHAN          4       // 3,3 V für Panel / I2C-Pegel
#define LDO_PANEL_MV            3300

/* --- MIPI-DSI / DPI (Werte aus dem getesteten Elecrow-IDF-Beispiel) --- */
#define DSI_LANES               2
#define DSI_LANE_MBPS           900
#define DPI_CLK_MHZ             51
#define DPI_HSYNC               70
#define DPI_HBP                 160
#define DPI_HFP                 160
#define DPI_VSYNC               10
#define DPI_VBP                 23
#define DPI_VFP                 12

/* --- Hintergrundbeleuchtung --- */
#define BL_GPIO                 GPIO_NUM_31
#define BL_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER           LEDC_TIMER_0
#define BL_LEDC_CHANNEL         LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ         30000
#define BL_LEDC_RES             LEDC_TIMER_11_BIT
/* Elecrow-Abbildung: 1..100 % -> duty = p * 18 + 200 (max. 2000 von 2047) */
#define BL_DUTY_MIN             200
#define BL_DUTY_STEP            18

/* --- Touch GT911 an I2C0 --- */
#define TOUCH_I2C_PORT          I2C_NUM_0
#define TOUCH_I2C_SDA           GPIO_NUM_45
#define TOUCH_I2C_SCL           GPIO_NUM_46
#define TOUCH_I2C_HZ            400000
#define TOUCH_RST               GPIO_NUM_40
#define TOUCH_INT               GPIO_NUM_42

static const board_info_t s_info = {
    .name = "crowpanel_p4_9in",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
};

static esp_ldo_channel_handle_t s_ldo_phy;
static esp_ldo_channel_handle_t s_ldo_panel;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_touch_handle_t s_touch;
static uint8_t s_brightness;

const board_info_t *board_info(void)
{
    return &s_info;
}

esp_lcd_panel_handle_t board_panel(void)       { return s_panel; }
esp_lcd_panel_io_handle_t board_panel_io(void) { return s_panel_io; }
esp_lcd_touch_handle_t board_touch(void)       { return s_touch; }
uint8_t board_backlight_get(void)              { return s_brightness; }

static esp_err_t power_init(void)
{
    const esp_ldo_channel_config_t phy_cfg = {
        .chan_id = LDO_MIPI_PHY_CHAN,
        .voltage_mv = LDO_MIPI_PHY_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&phy_cfg, &s_ldo_phy), TAG, "LDO3");

    const esp_ldo_channel_config_t panel_cfg = {
        .chan_id = LDO_PANEL_CHAN,
        .voltage_mv = LDO_PANEL_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&panel_cfg, &s_ldo_panel), TAG, "LDO4");

    ESP_LOGI(TAG, "LDO3 %d mV, LDO4 %d mV aktiv", LDO_MIPI_PHY_MV, LDO_PANEL_MV);
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC-Timer");

    const ledc_channel_config_t channel = {
        .gpio_num = BL_GPIO,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "LEDC-Kanal");
    s_brightness = 0;
    return ESP_OK;
}

esp_err_t board_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    const uint32_t duty = (percent == 0) ? 0 : (uint32_t)percent * BL_DUTY_STEP + BL_DUTY_MIN;
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL), TAG, "update");
    s_brightness = percent;
    return ESP_OK;
}

static esp_err_t display_init(void)
{
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
        // Rev < 3.0 kennt nur PLL_F20M/F25M/RC_FAST; der IDF-6-Default (XTAL) gibt es dort nicht.
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY,
#else
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT,
#endif
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus), TAG, "DSI-Bus");

    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_panel_io), TAG, "DBI-IO");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_pulse_width = DPI_HSYNC,
            .hsync_back_porch = DPI_HBP,
            .hsync_front_porch = DPI_HFP,
            .vsync_pulse_width = DPI_VSYNC,
            .vsync_back_porch = DPI_VBP,
            .vsync_front_porch = DPI_VFP,
        },
    };

    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,   // Reset per Befehl, wie im Elecrow-Beispiel
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(s_panel_io, &dev_cfg, &s_panel), TAG, "EK79007");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Panel-Reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel-Init");

    ESP_LOGI(TAG, "Display %dx%d initialisiert", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

static esp_err_t touch_try_addr(uint16_t addr)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    io_cfg.scl_speed_hz = TOUCH_I2C_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &io), TAG, "Touch-IO");

    esp_lcd_touch_io_gt911_config_t gt911_cfg = { .dev_addr = addr };
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .driver_data = &gt911_cfg,
    };
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        esp_lcd_panel_io_del(io);
        s_touch = NULL;
    }
    return err;
}

static esp_err_t touch_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "I2C0");

    if (touch_try_addr(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 an 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS);
        return ESP_OK;
    }
    if (touch_try_addr(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 an 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "Profil %s", s_info.name);
    ESP_RETURN_ON_ERROR(power_init(), TAG, "Stromversorgung");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "Hintergrundbeleuchtung");
    ESP_RETURN_ON_ERROR(display_init(), TAG, "Display");

    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "Kein Touch-Controller gefunden – weiter ohne Touch");
    }
    return ESP_OK;
}

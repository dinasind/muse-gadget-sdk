/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 *
 * Espressif ESP32-S31-Korvo-1 v1.1: ESP32-S31-WROOM-3 with 16 MB flash,
 * 16 MB octal PSRAM, an 800x480 ST7262E43 RGB panel, GT1151 touch, ES8389
 * stereo codec, two analog microphones, two NS4150B amplifiers and BOOT on
 * GPIO61. The panel backlight is always on; LCD DISP on GPIO38 gates output.
 *
 * Pins and timings are ported from xiaozhi-esp32's
 * main/boards/espressif/esp32-s31-korvo-1 at revision
 * c7241272f2d5fd140c77542f3cf12d09e717fc2f, which cites Espressif's
 * esp-dev-kits factory demo and board schematic. ESP32-S31 requires ESP-IDF
 * 6.1 or newer.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt1151.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_link.h"
#include "muse_mem.h"

#define LCD_W 800
#define LCD_H 480

#define I2C_SDA GPIO_NUM_0
#define I2C_SCL GPIO_NUM_1
#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_3
#define I2S_WS GPIO_NUM_4
#define I2S_DOUT GPIO_NUM_5
#define I2S_DIN GPIO_NUM_6
#define PA_EN GPIO_NUM_7
#define BOOT_GPIO GPIO_NUM_61
#define LCD_DISP GPIO_NUM_38

static const char *TAG = "board";
static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static SemaphoreHandle_t s_lv_lock;
static SemaphoreHandle_t s_started;
static esp_err_t s_start_err;
static muse_gpio_button_t s_boot;
static bool s_touch_pressed;

static esp_err_t init(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "I2C bus");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "BOOT button");
    /* BOOT can still be held after reset; do not report it as a fresh press. */
    s_boot.pressed = gpio_get_level(BOOT_GPIO) == 0;
    return ESP_OK;
}

static esp_err_t panel_start(void)
{
    const esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = LCD_W,
            .v_res = LCD_H,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        /* Eight lines is stable on the reference board and limits internal SRAM use. */
        .bounce_buffer_size_px = LCD_W * 8,
        .dma_burst_size = 64,
        .hsync_gpio_num = GPIO_NUM_44,
        .vsync_gpio_num = GPIO_NUM_45,
        .de_gpio_num = GPIO_NUM_43,
        .pclk_gpio_num = GPIO_NUM_40,
        .disp_gpio_num = LCD_DISP,
        .data_gpio_nums = {
            GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
            GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15,
            GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19,
            GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_35, GPIO_NUM_36,
        },
        .flags.fb_in_psram = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG, "RGB panel");
    return esp_lcd_panel_init(s_panel);
}

static esp_err_t touch_start(void)
{
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch I2C IO");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_W - 1,
        .y_max = LCD_H - 1,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
    };
    return esp_lcd_touch_new_i2c_gt1151(io, &tp_cfg, &s_touch);
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    /* LVGL renders directly into the RGB controller's continuously scanned FB. */
    lv_display_flush_ready(disp);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        return;
    }
    esp_lcd_touch_point_data_t point;
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) != ESP_OK || !count) {
        s_touch_pressed = false;
        return;
    }
    data->point.x = point.x < LCD_W ? point.x : LCD_W - 1;
    data->point.y = point.y < LCD_H ? point.y : LCD_H - 1;
    data->state = LV_INDEV_STATE_PRESSED;
    if (!s_touch_pressed && muse_link_state() == MUSE_LINK_CONFIRM && muse_link_talk_press()) {
        ESP_LOGI(TAG, "pairing confirmed by screen tap");
    }
    s_touch_pressed = true;
}

static esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(panel_start(), TAG, "panel");
    ESP_RETURN_ON_ERROR(touch_start(), TAG, "touch");

    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb), TAG, "frame buffer");
    ESP_RETURN_ON_FALSE(fb, ESP_ERR_NO_MEM, TAG, "no frame buffer");
    memset(fb, 0, LCD_W * LCD_H * 2);

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(LCD_W, LCD_H);
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "LVGL display");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, fb, NULL, LCD_W * LCD_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush);

    s_indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(s_indev, ESP_ERR_NO_MEM, TAG, "LVGL touch");
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read);
    lv_indev_set_display(s_indev, s_disp);
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    s_start_err = display_init();
    xSemaphoreGive(s_started);
    if (s_start_err != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        xSemaphoreTakeRecursive(s_lv_lock, portMAX_DELAY);
        uint32_t ms = lv_timer_handler();
        xSemaphoreGiveRecursive(s_lv_lock);
        ms = ms < 5 ? 5 : ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    s_lv_lock = xSemaphoreCreateRecursiveMutex();
    s_started = xSemaphoreCreateBinary();
    if (!s_lv_lock || !s_started ||
        xTaskCreatePinnedToCoreWithCaps(lvgl_task, "lvgl", 8192, NULL, MUSE_UI_PRIORITY,
                                        NULL, MUSE_UI_CORE, MUSE_BIG_CAPS) != pdPASS) {
        return NULL;
    }
    xSemaphoreTake(s_started, portMAX_DELAY);
    if (s_start_err != ESP_OK) {
        return NULL;
    }
    *touch = s_indev;
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lv_lock,
                                   timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lv_lock);
}

static void set_brightness(int pct)
{
    /* The LCD daughterboard has no controllable backlight; DISP blanks the panel. */
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, pct > 0);
    }
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "I2S channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "I2S RX enable");

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8389_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PA_EN,
        .use_mclk = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec = es8389_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8389 not responding");

    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec,
        .data_if = data_if,
    };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec,
        .data_if = data_if,
    };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){&s_boot}, 1, timeout_ms);
}

static esp_err_t power_off(void)
{
    /* GPIO61 is a high-power digital pad, not an RTC wake pin. The board has no
     * software power latch, so blank the panel and leave RESET as the way back. */
    set_brightness(0);
    return ESP_ERR_NOT_SUPPORTED;
}

static const muse_board_t s_board = {
    .name = "Espressif ESP32-S31-Korvo-1",
    .width = LCD_W,
    .height = LCD_H,
    .avatar_px = 288,
    .round = false,
    .touch = true,
    .diagonal_in = 4.3f,
    .talk_button = "boot",
    .talk_hint = {LV_ALIGN_BOTTOM_LEFT, 12, -8},
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = -1,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}

# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0.

"""Compile the board's actual RGB callbacks against an IDF bounce-reader model."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "components/muse/boards/board_espressif_s31_korvo_1.c"


def function(source, signature):
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


class S31RgbFlushTest(unittest.TestCase):
    def test_buffer_configuration(self):
        source = BOARD.read_text()
        self.assertIn(".num_fbs = 2,", source)
        self.assertIn(".bounce_buffer_size_px = LCD_W * 8,", source)
        self.assertIn(".pclk_hz = 16 * 1000 * 1000,", source)
        self.assertIn("s_panel, 2, &fb0, &fb1", source)
        self.assertIn("s_disp, fb1, fb0, LCD_W * LCD_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT", source)
        self.assertNotIn("memset(", source)  # fb0 is already being scanned
        self.assertIn(".on_frame_buf_complete = frame_buf_complete", source)
        self.assertLess(source.index("esp_lcd_rgb_panel_register_event_callbacks("),
                        source.index("return esp_lcd_panel_init(s_panel)"))

    def test_init_failure_stops_callback_before_task_exit(self):
        task = function(BOARD.read_text(), "static void lvgl_task(")
        self.assertIn("s_start_err != ESP_OK && s_panel", task)
        self.assertLess(task.index("esp_lcd_panel_del(s_panel)"),
                        task.index("vTaskDelete(NULL)"))

    def test_flush_fence_interleavings(self):
        source = BOARD.read_text()
        callbacks = (function(source, "static bool IRAM_ATTR frame_buf_complete(") +
                     function(source, "static void flush("))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define IRAM_ATTR
#define LCD_W 800
#define LCD_H 480
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define ESP_ERROR_CHECK(expr) assert((expr) == 0)
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *esp_lcd_panel_handle_t;
typedef int esp_lcd_rgb_panel_event_data_t;
typedef int lv_display_t;
typedef int lv_area_t;
static void *s_panel;
static uint8_t buffers[2];
static uint8_t *reader = &buffers[0], *submitted = &buffers[0];
static unsigned notifications, waits, boundaries, draws, ready;
static bool last, delayed_old, batch, complete_during_draw;
static bool frame_buf_complete(esp_lcd_panel_handle_t,
                               const esp_lcd_rgb_panel_event_data_t *, void *);
static void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *wake)
{
    assert(task == (void *)1);
    notifications++;
    *wake = pdTRUE;
}
static void boundary(void)
{
    reader = submitted;  /* IDF latches BEFORE calling the user callback. */
    boundaries++;
    assert(frame_buf_complete(NULL, NULL, (void *)1));
}
static unsigned ulTaskNotifyTake(int clear, unsigned timeout)
{
    if (!timeout) {
        assert(clear == pdTRUE);
        unsigned result = notifications;
        notifications = 0;
        return result;
    }
    assert(clear == pdFALSE && timeout == portMAX_DELAY);
    waits++;
    if (!notifications) {
        if (delayed_old) {
            /* Other core latched OLD fb before submit but not yet notified. */
            delayed_old = false;
            frame_buf_complete(NULL, NULL, (void *)1);
        } else {
            boundary();
            if (batch) boundary();
        }
    }
    unsigned result = notifications;
    notifications--;
    return result;
}
static bool lv_display_flush_is_last(lv_display_t *disp)
{
    (void)disp;
    return last;
}
static int esp_lcd_panel_draw_bitmap(void *panel, int x1, int y1,
                                     int x2, int y2, uint8_t *px)
{
    (void)panel;
    assert(x1 == 0 && y1 == 0 && x2 == LCD_W && y2 == LCD_H);
    assert(px != reader); /* Must never render/submit the current front. */
    submitted = px;
    draws++;
    if (complete_during_draw) boundary();
    return 0;
}
static void lv_display_flush_ready(lv_display_t *disp)
{
    (void)disp;
    if (last) {
        assert(waits == 2);
        assert(reader == submitted); /* Previous front is now reusable. */
        assert(boundaries >= 1);
    } else {
        assert(waits == 0 && draws == 0);
    }
    ready++;
}
'''
        main = r'''
int main(void)
{
    for (unsigned scenario = 0; scenario < 16; scenario++) {
        reader = submitted = &buffers[0];
        for (unsigned frame = 0; frame < 8; frame++) {
            waits = boundaries = draws = ready = 0;
            notifications = (scenario & 1) ? 7 : 0;
            delayed_old = (scenario & 2) != 0;
            batch = (scenario & 4) != 0;
            complete_during_draw = (scenario & 8) != 0 && !delayed_old;
            uint8_t *back = reader == &buffers[0] ? &buffers[1] : &buffers[0];
            last = false;
            flush(NULL, NULL, back); /* A DIRECT frame can contain many areas. */
            flush(NULL, NULL, back);
            assert(ready == 2);
            last = true;
            flush(NULL, NULL, back);
            assert(ready == 3 && draws == 1 && reader == back);
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / "rgb_flush.c"
            binary = Path(tmp) / "rgb_flush"
            cfile.write_text(harness + callbacks + main)
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")),
                            "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(cfile), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()

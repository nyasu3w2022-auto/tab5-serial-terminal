/*
 * display.cpp — LVGL-based terminal display rendering.
 *
 * SPDX-License-Identifier: MIT
 */

#include "display.h"
#include "terminal.h"
#include "usb_serial.h"

#include <inttypes.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include "lvgl_port.h"
#include "lvgl_port_disp.h"
#include "lvgl_port_touch.h"
#include "lvgl.h"
#include "font/lv_font_fmt_txt.h"

static const char *TAG = "display";

static lv_obj_t   *term_canvas  = NULL;
static lv_obj_t   *status_label = NULL;
static lv_timer_t *cursor_timer = NULL;
static lv_obj_t   *row_canvases[TERM_ROWS]    = {};
static uint8_t    *row_canvas_bufs[TERM_ROWS] = {};

static constexpr uint32_t LCD_H_RES = 720;
static constexpr uint32_t LCD_V_RES = 1280;
static lv_display_t *s_lvgl_disp        = nullptr;
static lv_indev_t   *s_lvgl_touch_indev = nullptr;

static void term_rebuild_row(int r)
{
    uint16_t *buf = (uint16_t *)row_canvas_bufs[r];
    if (buf == NULL) return;

    const lv_font_t *font = &lv_font_unscii_16;
    const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *)font->dsc;
    const int stride = LVGL_W;

    for (int c = 0; c < TERM_COLS; c++) {
        TermCell *cell = &term_buffer[r][c];
        bool is_cursor = (r == cursor_row && c == cursor_col &&
                          cursor_visible && cursor_blink_state);

        uint8_t fg_idx = cell->fg & 15;
        uint8_t bg_idx = cell->bg & 15;
        lv_color_t fg_col = is_cursor ? lv_color_black() : TERM_COLORS[fg_idx];
        lv_color_t bg_col = is_cursor ? lv_color_white() : TERM_COLORS[bg_idx];
        uint16_t fg16 = lv_color_to_u16(fg_col);
        uint16_t bg16 = lv_color_to_u16(bg_col);

        char ch = cell->ch;
        uint32_t letter = (ch >= 0x20 && ch < 0x7F) ? (uint32_t)(uint8_t)ch : (uint32_t)' ';
        lv_font_glyph_dsc_t g_dsc;
        bool found = lv_font_get_glyph_dsc(font, &g_dsc, letter, 0);

        uint16_t *cell_base = buf + c * TERM_FONT_W;

        if (!found || g_dsc.box_w == 0 || g_dsc.box_h == 0) {
            for (int y = 0; y < TERM_FONT_H; y++) {
                uint16_t *row_ptr = cell_base + y * stride;
                for (int x = 0; x < TERM_FONT_W; x++) row_ptr[x] = bg16;
            }
            continue;
        }

        uint32_t gid = g_dsc.gid.index;
        const lv_font_fmt_txt_glyph_dsc_t *gdsc = &fdsc->glyph_dsc[gid];
        const uint8_t *bitmap = &fdsc->glyph_bitmap[gdsc->bitmap_index];

        int bw = g_dsc.box_w;
        int bh = g_dsc.box_h;
        int ox = g_dsc.ofs_x;
        int baseline_y   = (TERM_FONT_H - 1) - (int)font->base_line;
        int cell_y_start = baseline_y - (int)g_dsc.ofs_y - bh + 1;

        for (int y = 0; y < TERM_FONT_H; y++) {
            uint16_t *row_ptr = cell_base + y * stride;
            for (int x = 0; x < TERM_FONT_W; x++) row_ptr[x] = bg16;
        }

        for (int gy = 0; gy < bh; gy++) {
            int cy = cell_y_start + gy;
            if (cy < 0 || cy >= TERM_FONT_H) continue;
            uint16_t *row_ptr = cell_base + cy * stride;
            for (int gx = 0; gx < bw; gx++) {
                int cx = ox + gx;
                if (cx < 0 || cx >= TERM_FONT_W) continue;
                int bit_idx  = gy * bw + gx;
                int byte_idx = bit_idx >> 3;
                int bit_pos  = 7 - (bit_idx & 7);
                uint8_t bit  = (bitmap[byte_idx] >> bit_pos) & 1;
                row_ptr[cx] = bit ? fg16 : bg16;
            }
        }

    }

    // Notify LVGL that the canvas pixel buffer has been updated
    if (row_canvases[r]) {
        lv_canvas_set_buffer(row_canvases[r], row_canvas_bufs[r], LVGL_W, TERM_FONT_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(row_canvases[r]);
    }
}

void term_refresh_display(void)
{
    if (term_canvas == NULL) return;

    static int prev_cursor_row = -1;
    static int prev_cursor_col = -1;
    if (prev_cursor_row != cursor_row || prev_cursor_col != cursor_col) {
        term_mark_dirty(prev_cursor_row);
        term_mark_dirty(cursor_row);
        prev_cursor_row = cursor_row;
        prev_cursor_col = cursor_col;
    } else {
        term_mark_dirty(cursor_row);
    }

    lvgl_port_lock(0);
    for (int r = 0; r < TERM_ROWS; r++) {
        if (row_dirty[r]) {
            term_rebuild_row(r);
            row_dirty[r] = false;
        }
    }
    lvgl_port_unlock();
}

static void term_update_status(const char *msg)
{
    if (status_label == NULL) return;
    lvgl_port_lock(0);
    lv_label_set_text(status_label, msg);
    lvgl_port_unlock();
}

void update_status_bar(void)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             " USB:%s  Baud:%"PRIu32"  Ctrl+C=Clear  Ctrl+B=Baud",
             usb_is_connected() ? "Connected" : "Waiting...",
             usb_get_baud_rate());
    term_update_status(buf);
}

static void cursor_blink_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!cursor_visible) {
        cursor_blink_state = false;
        return;
    }
    cursor_blink_state = !cursor_blink_state;
    term_refresh_display();
}

void ui_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    term_canvas = lv_obj_create(scr);
    lv_obj_set_pos(term_canvas, 0, 0);
    lv_obj_set_size(term_canvas, LVGL_W, LVGL_H - STATUS_BAR_H);
    lv_obj_set_style_bg_color(term_canvas, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(term_canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(term_canvas, 0, 0);
    lv_obj_set_style_pad_all(term_canvas, 0, 0);
    lv_obj_clear_flag(term_canvas, LV_OBJ_FLAG_SCROLLABLE);

    for (int r = 0; r < TERM_ROWS; r++) {
        size_t buf_size = LV_CANVAS_BUF_SIZE(LVGL_W, TERM_FONT_H, 16, 4);
        row_canvas_bufs[r] = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (row_canvas_bufs[r] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate canvas buffer for row %d", r);
            continue;
        }
        row_canvases[r] = lv_canvas_create(term_canvas);
        lv_canvas_set_buffer(row_canvases[r], row_canvas_bufs[r], LVGL_W, TERM_FONT_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(row_canvases[r], 0, r * TERM_FONT_H);
        lv_obj_set_size(row_canvases[r], LVGL_W, TERM_FONT_H);
        lv_canvas_fill_bg(row_canvases[r], lv_color_black(), LV_OPA_COVER);
        row_dirty[r] = true;
    }

    status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(status_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_color(status_label, lv_color_make(0, 200, 0), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_COVER, 0);
    lv_obj_set_pos(status_label, 0, LVGL_H - STATUS_BAR_H);
    lv_obj_set_size(status_label, LVGL_W, STATUS_BAR_H);
    lv_label_set_text(status_label, " TAB5 Serial Terminal - Initializing...");

    cursor_timer = lv_timer_create(cursor_blink_cb, 500, NULL);

    lvgl_port_unlock();
}

esp_err_t app_lcd_lvgl_init(m5::tab5::m5tab5_component &board)
{
    if (s_lvgl_disp != nullptr) return ESP_OK;

    esp_lcd_panel_handle_t panel_handle = board.lcd_panel();
    if (panel_handle == nullptr) {
        ESP_LOGE(TAG, "LCD panel handle unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_cfg_t lvgl_cfg   = {};
    lvgl_cfg.task_priority     = 6;
    lvgl_cfg.task_stack        = 16384;
    lvgl_cfg.task_affinity     = 1;
    lvgl_cfg.task_max_sleep_ms = 500;
    lvgl_cfg.timer_period_ms   = 5;

    esp_err_t ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    lvgl_disp_cfg_t disp_cfg    = {};
    disp_cfg.panel_handle       = panel_handle;
    disp_cfg.hres               = LCD_H_RES;
    disp_cfg.vres               = LCD_V_RES;
    disp_cfg.buffer_size        = LCD_H_RES * LCD_V_RES;
    disp_cfg.color_format       = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.full_refresh = 0;
    disp_cfg.flags.direct_mode  = 1;
    disp_cfg.flags.buff_spiram  = 1;
    disp_cfg.flags.sw_rotate    = 1;

    lvgl_disp_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.sw_rotation         = LV_DISPLAY_ROTATION_90;
    dsi_cfg.flags.avoid_tearing = 1;
    dsi_cfg.flags.use_ppa       = 1;

    s_lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (s_lvgl_disp == nullptr) {
        ESP_LOGE(TAG, "Failed to add LVGL DSI display");
        return ESP_FAIL;
    }

    esp_lcd_touch_handle_t touch_handle = board.touch_panel();
    if (touch_handle != nullptr) {
        // GT911 physical resolution: 720 x 1280 (portrait)
        // LVGL logical resolution after 90-degree SW rotation: 1280 x 720 (landscape)
        // The lvgl_port touch callback applies ROTATION_90 transform:
        //   new_x = hres - 1 - (touch_y * scale.y)
        //   new_y = touch_x * scale.x
        // With scale = 1.0 (default), GT911 raw coords map correctly:
        //   touch_y (0..1280) -> new_x (0..1279)  OK
        //   touch_x (0..720)  -> new_y (0..719)   OK
        // No additional scaling is needed.
        lvgl_touch_cfg_t touch_cfg = {};
        touch_cfg.disp             = s_lvgl_disp;
        touch_cfg.handle           = touch_handle;
        s_lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
    }

    return ESP_OK;
}

#pragma once
/*
 * terminal.h — Terminal buffer management and VT100 escape sequence parser.
 *
 * This module owns:
 *   - Screen geometry constants (TERM_ROWS, TERM_COLS, font dimensions)
 *   - TermCell type and terminal buffer
 *   - Cursor state (position, visibility, blink)
 *   - SGR color palette (16 colors)
 *   - VT100 byte-at-a-time parser (vt100_process_byte)
 *   - Terminal buffer operations (clear, scroll, put_char)
 *
 * Dependencies: LVGL (for lv_color_t), ESP-IDF (for FreeRTOS types)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

// ==============================================================
// Screen Geometry
// ==============================================================
// NOTE: TAB5 display is physically 720x1280 (portrait), but LVGL uses
// LV_DISPLAY_ROTATION_90 with PPA, so the LVGL logical coordinate space
// is 1280x720 (landscape). All UI coordinates use the rotated values.
#define LVGL_W          1280   // LVGL logical width  (after 90-degree rotation)
#define LVGL_H          720    // LVGL logical height (after 90-degree rotation)
#define TERM_FONT_W     16
#define TERM_FONT_H     16
#define STATUS_BAR_H    20
#define TERM_COLS       (LVGL_W / TERM_FONT_W)                   // 80
#define TERM_ROWS       ((LVGL_H - STATUS_BAR_H) / TERM_FONT_H)  // 43

// ==============================================================
// Terminal Cell and Color Definitions
// ==============================================================

// ANSI/VT100 SGR color indices (0-7 standard, 8-15 bright)
// Index 0=Black 1=Red 2=Green 3=Yellow 4=Blue 5=Magenta 6=Cyan 7=White
// Index 8-15 = bright versions
extern const lv_color_t TERM_COLORS[16];

#define DEFAULT_FG  7   // White
#define DEFAULT_BG  0   // Black

typedef struct {
    uint32_t codepoint; // Unicode codepoint (0x20 = space/empty)
    uint8_t  fg;        // foreground color index 0-15
    uint8_t  bg;        // background color index 0-15
    uint8_t  bold;      // bold/bright flag
    uint8_t  wide;      // 1 = full-width (occupies 2 columns), 2 = right half of wide char
} TermCell;

// ==============================================================
// Terminal State (read by display module)
// ==============================================================
extern TermCell term_buffer[TERM_ROWS][TERM_COLS];
extern int      cursor_row;
extern int      cursor_col;
extern bool     cursor_visible;
extern bool     cursor_blink_state;
extern bool     row_dirty[TERM_ROWS];

// ==============================================================
// Terminal Buffer API
// ==============================================================

/** Clear entire screen, reset cursor and SGR attributes. */
void term_clear_all(void);

/** Mark a single row as needing redraw. */
void term_mark_dirty(int row);

/** Mark all rows as needing redraw (e.g. after Ctrl+L). */
void term_mark_all_dirty(void);

// ==============================================================
// VT100 Parser API
// ==============================================================

/**
 * @brief Register a callback for sending data back to the remote device.
 *
 * Used by the VT100 parser to respond to DSR (ESC[6n) and DA (ESC[c)
 * queries without depending directly on the USB module.
 *
 * @param cb  Function pointer: cb(data, len) sends len bytes to the device.
 *            Pass NULL to disable responses.
 */
typedef void (*vt100_tx_cb_t)(const uint8_t *data, size_t len);
void vt100_set_tx_cb(vt100_tx_cb_t cb);

/**
 * @brief Feed one byte to the VT100 parser.
 *
 * Call this for every byte received from the remote device.
 * Updates term_buffer, cursor position, and row_dirty[] flags.
 */
void vt100_process_byte(uint8_t byte);

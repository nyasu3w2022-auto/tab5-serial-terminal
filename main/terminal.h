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
#include <stddef.h>
#include "lvgl.h"

// ==============================================================
// Screen Geometry — Fixed display constants
// ==============================================================
// NOTE: TAB5 display is physically 720x1280 (portrait), but LVGL uses
// LV_DISPLAY_ROTATION_90 with PPA, so the LVGL logical coordinate space
// is 1280x720 (landscape). All UI coordinates use the rotated values.
#define LVGL_W          1280   // LVGL logical width  (after 90-degree rotation)
#define LVGL_H          720    // LVGL logical height (after 90-degree rotation)
#define STATUS_BAR_H    20

// Maximum terminal buffer dimensions (sized for the smallest font: 8px wide, 16px tall)
// Small (16px): 160 cols x 43 rows
// Large (28px):  91 cols x 25 rows
#define TERM_COLS_MAX   160
#define TERM_ROWS_MAX   43

// Fixed-size, volatile terminal history.  The backing store is allocated once
// from PSRAM at boot; failure leaves the normal terminal usable without
// scrollback.
#define TERM_SCROLLBACK_MAX_LINES 512

// Runtime font geometry — set by term_set_font_size(), read by display and VT100 parser.
// These are the *active* dimensions used for all terminal operations.
extern int g_term_font_w;   // half-width cell width in pixels
extern int g_term_font_h;   // cell height in pixels
extern int g_term_cols;     // active number of columns  (LVGL_W / g_term_font_w)
extern int g_term_rows;     // active number of rows     ((LVGL_H - STATUS_BAR_H) / g_term_font_h)

// Convenience macros that resolve to the runtime variables
#define TERM_FONT_W  g_term_font_w
#define TERM_FONT_H  g_term_font_h
#define TERM_COLS    g_term_cols
#define TERM_ROWS    g_term_rows

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
extern TermCell term_buffer[TERM_ROWS_MAX][TERM_COLS_MAX];
extern int      cursor_row;
extern int      cursor_col;
extern bool     cursor_visible;
extern bool     cursor_blink_state;
extern bool     row_dirty[TERM_ROWS_MAX];

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
// Volatile Scrollback API
// ==============================================================

/**
 * @brief Allocate the fixed PSRAM history buffer.
 *
 * This is deliberately a one-time allocation.  If it fails, scrollback stays
 * disabled while terminal parsing and rendering continue normally.
 *
 * @return true when the buffer is available (including a prior success).
 */
bool term_scrollback_init(void);

/** Return true when the PSRAM history buffer is available. */
bool term_scrollback_available(void);

/** Remove all retained history and return the display to the live screen. */
void term_scrollback_clear(void);

/** Number of physical lines currently retained in the history buffer. */
int term_scrollback_history_count(void);

/**
 * @brief Number of lines the visible top row is behind the live screen.
 *
 * Zero means the terminal is live.  A positive value means the display is
 * reading from scrollback plus the current screen.
 */
int term_scrollback_view_offset(void);

/** Return true while the display is viewing retained history rather than live output. */
bool term_scrollback_is_viewing(void);

/** Monotonically increases whenever the selected scrollback view changes. */
uint32_t term_scrollback_view_generation(void);

/**
 * @brief Move the view by physical terminal lines.
 *
 * Positive values move toward older output; negative values move toward the
 * live screen. The result is clamped to the retained history range.
 *
 * @return true when the visible position changed.
 */
bool term_scrollback_move(int line_delta);

/** Return to the current live screen. @return true when the view changed. */
bool term_scrollback_return_live(void);

/**
 * @brief Obtain one physical display row for the current view.
 *
 * The returned pointer is either a retained row or the current live
 * term_buffer row.  display_row must be in [0, TERM_ROWS).
 */
const TermCell *term_scrollback_display_row(int display_row);

/**
 * @brief Switch the active font size at runtime.
 *
 * Updates g_term_font_w, g_term_font_h, g_term_cols, g_term_rows.
 * Caller must call term_clear_all() and rebuild the display after this.
 *
 * @param font_w  Half-width cell width in pixels  (e.g. 8 or 14)
 * @param font_h  Cell height in pixels            (e.g. 16 or 28)
 */
void term_set_font_size(int font_w, int font_h);

/**
 * @brief Render locally transmitted keyboard input without feeding the RX
 *        VT100 parser or transmitting any extra data to the serial peer.
 *
 * These APIs keep a separate UTF-8 decode state, so local echo cannot corrupt
 * an incomplete escape sequence currently being received from the peer.
 */
void term_local_echo_text(const uint8_t *data, size_t len);
void term_local_echo_enter(void);
void term_local_echo_tab(void);
void term_local_echo_backspace(void);
void term_local_echo_delete(void);
void term_local_echo_cursor_left(void);
void term_local_echo_cursor_right(void);

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

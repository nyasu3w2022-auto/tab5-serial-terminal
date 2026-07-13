/*
 * M5Stack TAB5 Serial Terminal
 *
 * Features:
 *  - USB Host CDC-ACM with VCP support (CH34x, CP210x, FTDI)
 *  - Standard CDC-ACM fallback for devices like Raspberry Pi USB gadget
 *  - Bidirectional communication: keyboard -> USB TX, USB RX -> screen
 *  - VT100 terminal emulation (cursor movement, scroll region, erase, SGR 8-color)
 *  - ENTER key sends LF (\n)
 *  - Baud rate configurable via Ctrl+B
 *
 * Design:
 *  - usb_lib_task runs permanently (never calls usb_host_uninstall).
 *    When NO_CLIENTS is received it frees devices and keeps looping so
 *    the next connection can be handled without reinstalling the host.
 *  - new_dev_cb detects device connection and signals vcp_task via semaphore
 *  - vcp_task opens the device immediately without polling/timeout loops
 *  - VCP::open() is NOT used for non-VCP devices (Raspberry Pi etc.)
 *  - CDC_ACM_HOST_ERROR is ignored; only CDC_ACM_HOST_DEVICE_DISCONNECTED
 *    triggers reconnect
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#include "m5_tab5_component.h"
#include "m5_tab5_keyboard.h"
#include "m5tab5_pinmap.h"
#include "lvgl_port.h"
#include "lvgl_port_disp.h"
#include "lvgl_port_touch.h"
#include "lvgl.h"

// USB Host
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"
#include "usb/vcp.hpp"
using namespace esp_usb;

static const char *TAG = "terminal";

// ==============================================================
// Terminal Screen Configuration
// ==============================================================
#define TERM_FONT_W     16
#define TERM_FONT_H     16
#define STATUS_BAR_H    20
#define TERM_COLS       (720 / TERM_FONT_W)   // 45
#define TERM_ROWS       ((1280 - STATUS_BAR_H) / TERM_FONT_H)  // 78

// ==============================================================
// Terminal Cell and Color Definitions
// ==============================================================

// ANSI/VT100 SGR color indices (0-7 standard, 8-15 bright)
// Index 0=Black 1=Red 2=Green 3=Yellow 4=Blue 5=Magenta 6=Cyan 7=White
// Index 8-15 = bright versions
static const lv_color_t TERM_COLORS[16] = {
    lv_color_make(0,   0,   0),    // 0 Black
    lv_color_make(170, 0,   0),    // 1 Red
    lv_color_make(0,   170, 0),    // 2 Green
    lv_color_make(170, 170, 0),    // 3 Yellow
    lv_color_make(0,   0,   170),  // 4 Blue
    lv_color_make(170, 0,   170),  // 5 Magenta
    lv_color_make(0,   170, 170),  // 6 Cyan
    lv_color_make(170, 170, 170),  // 7 White (light gray)
    lv_color_make(85,  85,  85),   // 8 Bright Black (dark gray)
    lv_color_make(255, 85,  85),   // 9 Bright Red
    lv_color_make(85,  255, 85),   // 10 Bright Green
    lv_color_make(255, 255, 85),   // 11 Bright Yellow
    lv_color_make(85,  85,  255),  // 12 Bright Blue
    lv_color_make(255, 85,  255),  // 13 Bright Magenta
    lv_color_make(85,  255, 255),  // 14 Bright Cyan
    lv_color_make(255, 255, 255),  // 15 Bright White
};

#define DEFAULT_FG  7   // White
#define DEFAULT_BG  0   // Black

typedef struct {
    char    ch;     // character (space = empty)
    uint8_t fg;     // foreground color index 0-15
    uint8_t bg;     // background color index 0-15
    uint8_t bold;   // bold/bright flag
} TermCell;

static TermCell term_buffer[TERM_ROWS][TERM_COLS];

// Current cursor position
static int cursor_row = 0;
static int cursor_col = 0;

// Saved cursor position (ESC 7 / ESC[s)
static int saved_row = 0;
static int saved_col = 0;

// Current SGR attributes
static uint8_t cur_fg   = DEFAULT_FG;
static uint8_t cur_bg   = DEFAULT_BG;
static uint8_t cur_bold = 0;

// Scroll region (inclusive, 0-based)
static int scroll_top = 0;
static int scroll_bot = TERM_ROWS - 1;

// Cursor visibility
static bool cursor_visible = true;
static bool cursor_blink_state = true;  // true = shown

// LVGL objects
static lv_obj_t  *term_canvas  = NULL;  // canvas for terminal area
static lv_obj_t  *status_label = NULL;
static lv_timer_t *cursor_timer = NULL;
static lv_obj_t  *row_spangroups[TERM_ROWS] = {};  // one spangroup per terminal row
static bool       row_dirty[TERM_ROWS] = {};    // true if row needs redraw

static m5::tab5::m5tab5_component s_tab5_board;
static m5::M5Tab5Keyboard         s_keyboard;

// ==============================================================
// USB Serial state
// ==============================================================
#define USB_HOST_PRIORITY   20
#define USB_CDC_PRIORITY    19
#define USB_VCP_PRIORITY    18

static CdcAcmDevice *s_vcp_dev       = nullptr;
static bool          s_usb_connected = false;
static uint32_t      s_baud_rate     = 115200;

// ==============================================================
// Message queues / semaphores / event groups
// ==============================================================
typedef struct {
    uint8_t modifier;
    char    str[16];
} key_event_msg_t;
static QueueHandle_t s_key_queue = NULL;

typedef struct {
    uint8_t data[64];
    size_t  len;
} usb_rx_msg_t;
static QueueHandle_t s_usb_rx_queue = NULL;

// Semaphore: posted by new_dev_cb when a USB device appears
static SemaphoreHandle_t s_dev_present_sem = NULL;

// VID/PID of the most recently detected USB device (set by enum_filter_cb)
static volatile uint16_t s_dev_vid = 0;
static volatile uint16_t s_dev_pid = 0;

// Event group bits
#define USB_DEV_DISCONNECTED_BIT  BIT0
static EventGroupHandle_t s_usb_event_group = NULL;

static TaskHandle_t s_main_task_handle = NULL;

// ==============================================================
// Screen logging helper (thread-safe, from other tasks)
// ==============================================================
static QueueHandle_t s_screen_log_queue = NULL;
typedef struct {
    char msg[128];
} screen_log_msg_t;

static void screen_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void screen_log(const char *fmt, ...)
{
    if (s_screen_log_queue == NULL) return;
    screen_log_msg_t m = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(m.msg, sizeof(m.msg), fmt, args);
    va_end(args);
    xQueueSend(s_screen_log_queue, &m, 0);
}

// ==============================================================
// Terminal Buffer Management
// ==============================================================

static void term_cell_clear(TermCell *cell)
{
    cell->ch   = ' ';
    cell->fg   = DEFAULT_FG;
    cell->bg   = DEFAULT_BG;
    cell->bold = 0;
}

static void term_mark_dirty(int row)
{
    if (row >= 0 && row < TERM_ROWS) row_dirty[row] = true;
}

static void term_mark_all_dirty(void)
{
    for (int r = 0; r < TERM_ROWS; r++) row_dirty[r] = true;
}

static void term_clear_region(int row_start, int col_start, int row_end, int col_end)
{
    for (int r = row_start; r <= row_end; r++) {
        int cs = (r == row_start) ? col_start : 0;
        int ce = (r == row_end)   ? col_end   : TERM_COLS - 1;
        for (int c = cs; c <= ce; c++) {
            term_cell_clear(&term_buffer[r][c]);
        }
        term_mark_dirty(r);
    }
}

static void term_clear_all(void)
{
    term_clear_region(0, 0, TERM_ROWS - 1, TERM_COLS - 1);
    cursor_row = 0;
    cursor_col = 0;
    scroll_top = 0;
    scroll_bot = TERM_ROWS - 1;
    cur_fg     = DEFAULT_FG;
    cur_bg     = DEFAULT_BG;
    cur_bold   = 0;
}

// Scroll the scroll region up by n lines (content moves up, bottom fills with blank)
static void term_scroll_up(int n)
{
    if (n <= 0) return;
    if (n > (scroll_bot - scroll_top + 1)) n = scroll_bot - scroll_top + 1;
    for (int r = scroll_top; r <= scroll_bot - n; r++) {
        memcpy(term_buffer[r], term_buffer[r + n], sizeof(TermCell) * TERM_COLS);
        term_mark_dirty(r);
    }
    for (int r = scroll_bot - n + 1; r <= scroll_bot; r++) {
        for (int c = 0; c < TERM_COLS; c++) term_cell_clear(&term_buffer[r][c]);
        term_mark_dirty(r);
    }
}

// Scroll the scroll region down by n lines (content moves down, top fills with blank)
static void term_scroll_down(int n)
{
    if (n <= 0) return;
    if (n > (scroll_bot - scroll_top + 1)) n = scroll_bot - scroll_top + 1;
    for (int r = scroll_bot; r >= scroll_top + n; r--) {
        memcpy(term_buffer[r], term_buffer[r - n], sizeof(TermCell) * TERM_COLS);
        term_mark_dirty(r);
    }
    for (int r = scroll_top; r < scroll_top + n; r++) {
        for (int c = 0; c < TERM_COLS; c++) term_cell_clear(&term_buffer[r][c]);
        term_mark_dirty(r);
    }
}

static void term_newline(void)
{
    cursor_col = 0;
    if (cursor_row == scroll_bot) {
        term_scroll_up(1);
    } else {
        cursor_row++;
        if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1;
    }
}

static void term_put_char_raw(char c)
{
    if (cursor_row < 0 || cursor_row >= TERM_ROWS) return;
    if (cursor_col < 0 || cursor_col >= TERM_COLS) return;
    term_buffer[cursor_row][cursor_col].ch   = c;
    term_buffer[cursor_row][cursor_col].fg   = cur_bold ? (cur_fg | 8) : cur_fg;
    term_buffer[cursor_row][cursor_col].bg   = cur_bg;
    term_buffer[cursor_row][cursor_col].bold = cur_bold;
    term_mark_dirty(cursor_row);
    cursor_col++;
    if (cursor_col >= TERM_COLS) {
        // Auto-wrap
        cursor_col = 0;
        if (cursor_row == scroll_bot) {
            term_scroll_up(1);
        } else {
            cursor_row++;
            if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1;
        }
    }
}

// ==============================================================
// VT100 Escape Sequence Parser
// ==============================================================

typedef enum {
    VT_STATE_NORMAL = 0,
    VT_STATE_ESC,           // received ESC
    VT_STATE_CSI,           // received ESC [
    VT_STATE_CSI_PRIV,      // received ESC [ ?
    VT_STATE_ESC_HASH,      // received ESC #
    VT_STATE_ESC_CHARSET,   // received ESC ( or ESC ) - consume next byte
} vt_state_t;

#define VT_MAX_PARAMS  8
static vt_state_t vt_state    = VT_STATE_NORMAL;
static int        vt_params[VT_MAX_PARAMS];
static int        vt_num_params = 0;
static bool       vt_param_started = false;

static void vt_reset_params(void)
{
    for (int i = 0; i < VT_MAX_PARAMS; i++) vt_params[i] = -1;
    vt_num_params   = 0;
    vt_param_started = false;
}

static int vt_param(int idx, int def)
{
    if (idx < 0 || idx >= vt_num_params) return def;
    if (vt_params[idx] < 0) return def;
    return vt_params[idx];
}

static void vt_clamp_cursor(void)
{
    if (cursor_row < 0)          cursor_row = 0;
    if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1;
    if (cursor_col < 0)          cursor_col = 0;
    if (cursor_col >= TERM_COLS) cursor_col = TERM_COLS - 1;
}

// Map 256-color index to nearest 16-color index
// 0-7: standard colors (map directly)
// 8-15: bright colors (map directly)
// 16-231: 6x6x6 color cube -> nearest 8-color
// 232-255: grayscale -> nearest 8-color
static uint8_t color256_to_16(int idx)
{
    if (idx < 0)   idx = 0;
    if (idx > 255) idx = 255;
    if (idx < 16)  return (uint8_t)idx;  // direct map
    if (idx >= 232) {
        // Grayscale 232-255: map to black(0)/dark-gray(8)/light-gray(7)/white(15)
        int v = idx - 232;  // 0..23
        if (v < 6)  return 0;   // black
        if (v < 12) return 8;   // dark gray
        if (v < 18) return 7;   // light gray
        return 15;              // white
    }
    // 6x6x6 cube: index 16..231
    int cube = idx - 16;
    int b = cube % 6;
    int g = (cube / 6) % 6;
    int r = cube / 36;
    // Map each channel 0..5 -> 0..1 (threshold at 3)
    int rb = (r >= 3) ? 1 : 0;
    int gb = (g >= 3) ? 1 : 0;
    int bb = (b >= 3) ? 1 : 0;
    // Bright variant if any channel is high
    bool bright = (r >= 4 || g >= 4 || b >= 4);
    uint8_t base = (uint8_t)(rb | (gb << 1) | (bb << 2));
    return bright ? (base | 8) : base;
}

// SGR: set graphic rendition
static void vt_sgr(void)
{
    int n = (vt_num_params == 0) ? 1 : vt_num_params;
    int i = 0;
    while (i < n) {
        int p = vt_param(i, 0);
        if (p == 0) {
            cur_fg   = DEFAULT_FG;
            cur_bg   = DEFAULT_BG;
            cur_bold = 0;
        } else if (p == 1) {
            cur_bold = 1;
        } else if (p == 2 || p == 22) {
            cur_bold = 0;
        } else if (p >= 30 && p <= 37) {
            cur_fg = (uint8_t)(p - 30);
        } else if (p == 39) {
            cur_fg = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            cur_bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            cur_bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            cur_fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            cur_bg = (uint8_t)(p - 100 + 8);
        } else if (p == 38) {
            // 256-color or truecolor foreground
            int mode = vt_param(i + 1, -1);
            if (mode == 5 && i + 2 < n) {
                // ESC[38;5;Nm - 256 color
                cur_fg = color256_to_16(vt_param(i + 2, 0));
                i += 2;
            } else if (mode == 2 && i + 4 < n) {
                // ESC[38;2;R;G;Bm - truecolor: approximate to nearest 8-color
                int r = vt_param(i + 2, 0);
                int g = vt_param(i + 3, 0);
                int b = vt_param(i + 4, 0);
                bool bright = (r > 170 || g > 170 || b > 170);
                uint8_t base = (uint8_t)((r > 85 ? 1 : 0) | (g > 85 ? 2 : 0) | (b > 85 ? 4 : 0));
                cur_fg = bright ? (base | 8) : base;
                i += 4;
            }
        } else if (p == 48) {
            // 256-color or truecolor background
            int mode = vt_param(i + 1, -1);
            if (mode == 5 && i + 2 < n) {
                cur_bg = color256_to_16(vt_param(i + 2, 0));
                i += 2;
            } else if (mode == 2 && i + 4 < n) {
                int r = vt_param(i + 2, 0);
                int g = vt_param(i + 3, 0);
                int b = vt_param(i + 4, 0);
                bool bright = (r > 170 || g > 170 || b > 170);
                uint8_t base = (uint8_t)((r > 85 ? 1 : 0) | (g > 85 ? 2 : 0) | (b > 85 ? 4 : 0));
                cur_bg = bright ? (base | 8) : base;
                i += 4;
            }
        }
        i++;
    }
    ESP_LOGD(TAG, "SGR: fg=%d bg=%d bold=%d (params=%d)", cur_fg, cur_bg, cur_bold, n);
}

// Process a complete CSI sequence (ESC [ params final_char)
static void vt_process_csi(char final_ch)
{
    switch (final_ch) {
    // ---- Cursor movement ----
    case 'A': { // Cursor Up
        int n = vt_param(0, 1); if (n < 1) n = 1;
        cursor_row -= n;
        vt_clamp_cursor();
        break;
    }
    case 'B': { // Cursor Down
        int n = vt_param(0, 1); if (n < 1) n = 1;
        cursor_row += n;
        vt_clamp_cursor();
        break;
    }
    case 'C': { // Cursor Forward (Right)
        int n = vt_param(0, 1); if (n < 1) n = 1;
        cursor_col += n;
        vt_clamp_cursor();
        break;
    }
    case 'D': { // Cursor Back (Left)
        int n = vt_param(0, 1); if (n < 1) n = 1;
        cursor_col -= n;
        vt_clamp_cursor();
        break;
    }
    case 'E': { // Cursor Next Line
        int n = vt_param(0, 1); if (n < 1) n = 1;
        cursor_row += n; cursor_col = 0;
        vt_clamp_cursor();
        break;
    }
    case 'F': { // Cursor Previous Line
        int n = vt_param(0, 1); if (n < 1) n = 1;
        cursor_row -= n; cursor_col = 0;
        vt_clamp_cursor();
        break;
    }
    case 'G': { // Cursor Horizontal Absolute
        int n = vt_param(0, 1); if (n < 1) n = 1;
        cursor_col = n - 1;
        vt_clamp_cursor();
        break;
    }
    case 'H': // Cursor Position
    case 'f': { // Horizontal and Vertical Position
        int row = vt_param(0, 1); if (row < 1) row = 1;
        int col = vt_param(1, 1); if (col < 1) col = 1;
        cursor_row = row - 1;
        cursor_col = col - 1;
        vt_clamp_cursor();
        break;
    }
    case 'd': { // Line Position Absolute
        int n = vt_param(0, 1); if (n < 1) n = 1;
        cursor_row = n - 1;
        vt_clamp_cursor();
        break;
    }
    case 's': { // Save Cursor Position
        saved_row = cursor_row;
        saved_col = cursor_col;
        break;
    }
    case 'u': { // Restore Cursor Position
        cursor_row = saved_row;
        cursor_col = saved_col;
        vt_clamp_cursor();
        break;
    }
    // ---- Erase ----
    case 'J': { // Erase in Display
        int n = vt_param(0, 0);
        if (n == 0) {
            // Erase from cursor to end of screen
            term_clear_region(cursor_row, cursor_col, TERM_ROWS - 1, TERM_COLS - 1);
        } else if (n == 1) {
            // Erase from start of screen to cursor
            term_clear_region(0, 0, cursor_row, cursor_col);
        } else if (n == 2 || n == 3) {
            // Erase entire screen (keep cursor position)
            term_clear_region(0, 0, TERM_ROWS - 1, TERM_COLS - 1);
        }
        break;
    }
    case 'K': { // Erase in Line
        int n = vt_param(0, 0);
        if (n == 0) {
            // Erase from cursor to end of line
            for (int c = cursor_col; c < TERM_COLS; c++) term_cell_clear(&term_buffer[cursor_row][c]);
        } else if (n == 1) {
            // Erase from start of line to cursor
            for (int c = 0; c <= cursor_col; c++) term_cell_clear(&term_buffer[cursor_row][c]);
        } else if (n == 2) {
            // Erase entire line
            for (int c = 0; c < TERM_COLS; c++) term_cell_clear(&term_buffer[cursor_row][c]);
        }
        term_mark_dirty(cursor_row);
        break;
    }
    // ---- Scroll ----
    case 'S': { // Scroll Up (pan up, new lines at bottom)
        int n = vt_param(0, 1); if (n < 1) n = 1;
        term_scroll_up(n);
        break;
    }
    case 'T': { // Scroll Down (pan down, new lines at top)
        int n = vt_param(0, 1); if (n < 1) n = 1;
        term_scroll_down(n);
        break;
    }
    case 'r': { // Set Scrolling Region (top;bot, 1-based)
        int top = vt_param(0, 1); if (top < 1) top = 1;
        int bot = vt_param(1, TERM_ROWS); if (bot < 1 || bot > TERM_ROWS) bot = TERM_ROWS;
        if (top < bot) {
            scroll_top = top - 1;
            scroll_bot = bot - 1;
            cursor_row = 0;
            cursor_col = 0;
        }
        break;
    }
    // ---- Line insert/delete ----
    case 'L': { // Insert Line(s) - insert n blank lines at cursor, push down
        int n = vt_param(0, 1); if (n < 1) n = 1;
        // Only within scroll region
        if (cursor_row >= scroll_top && cursor_row <= scroll_bot) {
            int old_top = scroll_top;
            scroll_top = cursor_row;
            term_scroll_down(n);
            scroll_top = old_top;
        }
        break;
    }
    case 'M': { // Delete Line(s) - delete n lines at cursor, pull up
        int n = vt_param(0, 1); if (n < 1) n = 1;
        if (cursor_row >= scroll_top && cursor_row <= scroll_bot) {
            int old_top = scroll_top;
            scroll_top = cursor_row;
            term_scroll_up(n);
            scroll_top = old_top;
        }
        break;
    }
    // ---- Character insert/delete ----
    case '@': { // Insert Character(s) - insert n spaces at cursor, shift right
        int n = vt_param(0, 1); if (n < 1) n = 1;
        if (cursor_row < TERM_ROWS) {
            // Shift characters right by n
            for (int c = TERM_COLS - 1; c >= cursor_col + n; c--) {
                term_buffer[cursor_row][c] = term_buffer[cursor_row][c - n];
            }
            for (int c = cursor_col; c < cursor_col + n && c < TERM_COLS; c++) {
                term_cell_clear(&term_buffer[cursor_row][c]);
            }
            term_mark_dirty(cursor_row);
        }
        break;
    }
    case 'P': { // Delete Character(s) - delete n chars at cursor, shift left
        int n = vt_param(0, 1); if (n < 1) n = 1;
        if (cursor_row < TERM_ROWS) {
            for (int c = cursor_col; c < TERM_COLS - n; c++) {
                term_buffer[cursor_row][c] = term_buffer[cursor_row][c + n];
            }
            for (int c = TERM_COLS - n; c < TERM_COLS; c++) {
                term_cell_clear(&term_buffer[cursor_row][c]);
            }
            term_mark_dirty(cursor_row);
        }
        break;
    }
    case 'X': { // Erase Character(s) - erase n chars at cursor (no shift)
        int n = vt_param(0, 1); if (n < 1) n = 1;
        for (int c = cursor_col; c < cursor_col + n && c < TERM_COLS; c++) {
            term_cell_clear(&term_buffer[cursor_row][c]);
        }
        term_mark_dirty(cursor_row);
        break;
    }
    // ---- SGR ----
    case 'm': {
        vt_sgr();
        break;
    }
    // ---- Device status / cursor report (respond not needed for terminal) ----
    case 'n': // DSR - Device Status Report (ignore)
        break;
    case 'c': // DA - Device Attributes (ignore)
        break;
    default:
        ESP_LOGD(TAG, "VT100: unhandled CSI %d final='%c'", vt_param(0,-1), final_ch);
        break;
    }
}

// Process CSI ? sequences (private modes)
static void vt_process_csi_priv(char final_ch)
{
    int n = vt_param(0, 0);
    if (final_ch == 'h') {
        // Set mode
        if (n == 25) { cursor_visible = true; }
        // 1049: alternate screen buffer (ignore)
        // 1: application cursor keys (ignore)
        // 7: auto-wrap (always on, ignore)
    } else if (final_ch == 'l') {
        // Reset mode
        if (n == 25) { cursor_visible = false; }
    }
}

// Main byte processor: feed one byte at a time
static void vt100_process_byte(uint8_t byte)
{
    char c = (char)byte;

    switch (vt_state) {
    case VT_STATE_NORMAL:
        if (c == 0x1B) {  // ESC
            vt_state = VT_STATE_ESC;
        } else if (c == '\n') {
            // LF: move down and reset column (implicit CR+LF for internal messages)
            cursor_col = 0;
            if (cursor_row == scroll_bot) {
                term_scroll_up(1);
            } else {
                cursor_row++;
                if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1;
            }
        } else if (c == '\r') {
            cursor_col = 0;
        } else if (c == '\b' || c == 0x7F) {
            if (cursor_col > 0) {
                cursor_col--;
                term_cell_clear(&term_buffer[cursor_row][cursor_col]);
                term_mark_dirty(cursor_row);
            }
        } else if (c == '\t') {
            int next_tab = (cursor_col + 8) & ~7;
            if (next_tab >= TERM_COLS) {
                cursor_col = TERM_COLS - 1;
            } else {
                cursor_col = next_tab;
            }
        } else if (c == '\a') {
            // Bell - ignore
        } else if (c == 0x0E || c == 0x0F) {
            // SO/SI (charset shift) - ignore
        } else if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
            term_put_char_raw(c);
        }
        // Bytes >= 0x80 (UTF-8 multibyte) are ignored (no Japanese font yet)
        break;

    case VT_STATE_ESC:
        if (c == '[') {
            vt_state = VT_STATE_CSI;
            vt_reset_params();
        } else if (c == '7') {
            // Save cursor
            saved_row = cursor_row;
            saved_col = cursor_col;
            vt_state = VT_STATE_NORMAL;
        } else if (c == '8') {
            // Restore cursor
            cursor_row = saved_row;
            cursor_col = saved_col;
            vt_clamp_cursor();
            vt_state = VT_STATE_NORMAL;
        } else if (c == 'c') {
            // Full reset (RIS)
            term_clear_all();
            vt_state = VT_STATE_NORMAL;
        } else if (c == 'D') {
            // Index: move cursor down, scroll if at bottom
            if (cursor_row == scroll_bot) {
                term_scroll_up(1);
            } else {
                cursor_row++;
                if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1;
            }
            vt_state = VT_STATE_NORMAL;
        } else if (c == 'M') {
            // Reverse Index: move cursor up, scroll down if at top
            if (cursor_row == scroll_top) {
                term_scroll_down(1);
            } else {
                cursor_row--;
                if (cursor_row < 0) cursor_row = 0;
            }
            vt_state = VT_STATE_NORMAL;
        } else if (c == 'E') {
            // Next Line
            cursor_col = 0;
            if (cursor_row == scroll_bot) {
                term_scroll_up(1);
            } else {
                cursor_row++;
                if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1;
            }
            vt_state = VT_STATE_NORMAL;
        } else if (c == '#') {
            vt_state = VT_STATE_ESC_HASH;
        } else if (c == '(' || c == ')' || c == '*' || c == '+') {
            // Character set designation - consume next byte (charset id)
            vt_state = VT_STATE_ESC_CHARSET;
        } else {
            // Unknown ESC sequence - ignore
            vt_state = VT_STATE_NORMAL;
        }
        break;

    case VT_STATE_CSI:
        if (c == '?') {
            vt_state = VT_STATE_CSI_PRIV;
            vt_reset_params();
        } else if (c >= '0' && c <= '9') {
            if (!vt_param_started) {
                if (vt_num_params < VT_MAX_PARAMS) {
                    vt_params[vt_num_params] = c - '0';
                    vt_num_params++;
                }
                vt_param_started = true;
            } else {
                int idx = vt_num_params - 1;
                if (idx < VT_MAX_PARAMS) {
                    if (vt_params[idx] < 0) vt_params[idx] = 0;
                    vt_params[idx] = vt_params[idx] * 10 + (c - '0');
                }
            }
        } else if (c == ';') {
            if (!vt_param_started && vt_num_params < VT_MAX_PARAMS) {
                vt_params[vt_num_params] = -1;
                vt_num_params++;
            }
            vt_param_started = false;
        } else if (c >= 0x40 && c <= 0x7E) {
            // Final byte
            if (vt_param_started) {
                // last param already counted
            }
            vt_process_csi(c);
            vt_state = VT_STATE_NORMAL;
        } else {
            // Unexpected - reset
            vt_state = VT_STATE_NORMAL;
        }
        break;

    case VT_STATE_CSI_PRIV:
        if (c >= '0' && c <= '9') {
            if (!vt_param_started) {
                if (vt_num_params < VT_MAX_PARAMS) {
                    vt_params[vt_num_params] = c - '0';
                    vt_num_params++;
                }
                vt_param_started = true;
            } else {
                int idx = vt_num_params - 1;
                if (idx < VT_MAX_PARAMS) {
                    if (vt_params[idx] < 0) vt_params[idx] = 0;
                    vt_params[idx] = vt_params[idx] * 10 + (c - '0');
                }
            }
        } else if (c == ';') {
            if (!vt_param_started && vt_num_params < VT_MAX_PARAMS) {
                vt_params[vt_num_params] = -1;
                vt_num_params++;
            }
            vt_param_started = false;
        } else if (c >= 0x40 && c <= 0x7E) {
            vt_process_csi_priv(c);
            vt_state = VT_STATE_NORMAL;
        } else {
            vt_state = VT_STATE_NORMAL;
        }
        break;

    case VT_STATE_ESC_HASH:
        // ESC # n sequences (e.g. ESC#8 = fill screen with 'E')
        if (c == '8') {
            // DECALN: fill screen with 'E' (alignment test)
            for (int r = 0; r < TERM_ROWS; r++)
                for (int col = 0; col < TERM_COLS; col++) {
                    term_buffer[r][col].ch   = 'E';
                    term_buffer[r][col].fg   = DEFAULT_FG;
                    term_buffer[r][col].bg   = DEFAULT_BG;
                    term_buffer[r][col].bold = 0;
                }
            term_mark_all_dirty();
        }
        vt_state = VT_STATE_NORMAL;
        break;

    case VT_STATE_ESC_CHARSET:
        // Consume charset designator byte (e.g. 'B' for ASCII, '0' for graphics)
        // We don't implement charset switching; just return to normal state.
        vt_state = VT_STATE_NORMAL;
        break;
    }
}

// ==============================================================
// LVGL Display Update
// ==============================================================

// Recolor format: #RRGGBB text# (LVGL recolor syntax)
// We build a string for each row with color spans.
// Span text buffer: worst case all 45 cells different colors + cursor
// Each span text: up to TERM_COLS chars + NUL
#define SPAN_TEXT_BUF  (TERM_COLS + 2)

// Rebuild a single row's spangroup from term_buffer.
// Must be called with LVGL lock held.
static void term_rebuild_row(int r)
{
    lv_obj_t *sg = row_spangroups[r];
    if (sg == NULL) return;

    // Delete all existing spans
    uint32_t span_count = lv_spangroup_get_span_count(sg);
    for (uint32_t i = 0; i < span_count; i++) {
        lv_span_t *sp = lv_spangroup_get_child(sg, 0);
        if (sp) lv_spangroup_delete_span(sg, sp);
    }

    // Build new spans: group consecutive cells with same fg/bg/cursor
    static char span_text[SPAN_TEXT_BUF];
    uint8_t last_fg = 255, last_bg = 255;
    lv_span_t *cur_span = NULL;
    int span_len = 0;

    for (int c = 0; c <= TERM_COLS; c++) {
        // Flush span at end or on color change
        bool is_cursor = false;
        uint8_t fg = 0, bg = 0;

        if (c < TERM_COLS) {
            TermCell *cell = &term_buffer[r][c];
            fg = cell->fg & 15;
            bg = cell->bg & 15;
            is_cursor = (r == cursor_row && c == cursor_col &&
                         cursor_visible && cursor_blink_state);
            if (is_cursor) { uint8_t tmp = fg; fg = bg; bg = tmp; }
        }

        bool color_changed = (c == TERM_COLS) || (fg != last_fg || bg != last_bg);

        if (color_changed && cur_span != NULL && span_len > 0) {
            // Finalize current span text
            span_text[span_len] = '\0';
            lv_span_set_text(cur_span, span_text);
            cur_span = NULL;
            span_len = 0;
        }

        if (c >= TERM_COLS) break;

        if (cur_span == NULL) {
            cur_span = lv_spangroup_new_span(sg);
            if (cur_span == NULL) break;
            lv_style_t *style = lv_span_get_style(cur_span);
            lv_style_set_text_color(style, TERM_COLORS[fg]);
            // Note: lv_span style only supports text properties (color, font, decor, opa).
            // Background color per-span is not supported by LVGL v9 spangroup.
            // Row background is set via lv_obj_set_style_bg_color() on the spangroup object.
            last_fg = fg;
            last_bg = bg;
            span_len = 0;
        }

        // Append character to span text
        TermCell *cell = &term_buffer[r][c];
        char ch = cell->ch;
        span_text[span_len++] = (ch >= 0x20 && ch < 0x7F) ? ch : ' ';
    }

    // LV_SPAN_MODE_FIXED: refr_mode just calls refresh_self_size.
    // Explicitly invalidate to trigger redraw.
    lv_obj_invalidate(sg);
}

static void term_refresh_display(void)
{
    if (term_canvas == NULL) return;

    // Also mark cursor rows dirty (current and previously drawn)
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

static void update_status_bar(void)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             " USB:%s  Baud:%"PRIu32"  Ctrl+C=Clear  Ctrl+B=Baud",
             s_usb_connected ? "Connected" : "Waiting...",
             s_baud_rate);
    term_update_status(buf);
}

// ==============================================================
// Cursor blink timer callback
// ==============================================================
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

// ==============================================================
// LVGL UI Setup
// ==============================================================

static void ui_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Terminal area container
    term_canvas = lv_obj_create(scr);
    lv_obj_set_pos(term_canvas, 0, 0);
    lv_obj_set_size(term_canvas, 720, 1280 - STATUS_BAR_H);
    lv_obj_set_style_bg_color(term_canvas, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(term_canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(term_canvas, 0, 0);
    lv_obj_set_style_pad_all(term_canvas, 0, 0);
    lv_obj_clear_flag(term_canvas, LV_OBJ_FLAG_SCROLLABLE);

    // Create one spangroup per row for per-cell color support
    for (int r = 0; r < TERM_ROWS; r++) {
        row_spangroups[r] = lv_spangroup_create(term_canvas);
        lv_obj_set_style_text_font(row_spangroups[r], &lv_font_unscii_16, 0);
        lv_obj_set_style_text_letter_space(row_spangroups[r], 0, 0);
        lv_obj_set_style_text_line_space(row_spangroups[r], 0, 0);
        lv_obj_set_style_bg_color(row_spangroups[r], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(row_spangroups[r], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(row_spangroups[r], 0, 0);
        lv_obj_set_pos(row_spangroups[r], 0, r * TERM_FONT_H);
        lv_obj_set_size(row_spangroups[r], 720, TERM_FONT_H);
        lv_spangroup_set_overflow(row_spangroups[r], LV_SPAN_OVERFLOW_CLIP);
        lv_spangroup_set_mode(row_spangroups[r], LV_SPAN_MODE_FIXED);
        row_dirty[r] = true;  // initial draw
    }

    // Status bar
    status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(status_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_color(status_label, lv_color_make(0, 200, 0), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_COVER, 0);
    lv_obj_set_pos(status_label, 0, 1280 - STATUS_BAR_H);
    lv_obj_set_size(status_label, 720, STATUS_BAR_H);
    lv_label_set_text(status_label, " TAB5 Serial Terminal - Initializing...");

    // Cursor blink timer (500ms interval)
    cursor_timer = lv_timer_create(cursor_blink_cb, 500, NULL);

    lvgl_port_unlock();
}

// ==============================================================
// LCD/LVGL Initialization
// ==============================================================

static constexpr uint32_t LCD_H_RES = 720;
static constexpr uint32_t LCD_V_RES = 1280;

static lv_display_t *s_lvgl_disp        = nullptr;
static lv_indev_t   *s_lvgl_touch_indev = nullptr;

static esp_err_t app_lcd_lvgl_init(m5::tab5::m5tab5_component &board)
{
    if (s_lvgl_disp != nullptr) {
        return ESP_OK;
    }

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
        lvgl_touch_cfg_t touch_cfg = {};
        touch_cfg.disp             = s_lvgl_disp;
        touch_cfg.handle           = touch_handle;
        s_lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
    }

    return ESP_OK;
}

// ==============================================================
// Keyboard Event Handling
// ==============================================================

static void keyboard_event_cb(m5_tab5_key_event_t event, void *arg)
{
    if (event.type == M5_TAB5_KB_MODE_STRING && event.str_len > 0) {
        key_event_msg_t msg = {};
        msg.modifier = event.str_modifier;
        size_t copy_len = event.str_len < (sizeof(msg.str) - 1) ? event.str_len : (sizeof(msg.str) - 1);
        memcpy(msg.str, event.str_data, copy_len);
        msg.str[copy_len] = '\0';
        if (s_key_queue != NULL) {
            xQueueSend(s_key_queue, &msg, 0);
        }
    }
}

// ==============================================================
// USB VCP Callbacks
// ==============================================================

static bool usb_rx_cb(const uint8_t *data, size_t data_len, void *arg)
{
    if (s_usb_rx_queue == NULL || data_len == 0) return true;

    size_t offset = 0;
    while (offset < data_len) {
        usb_rx_msg_t msg = {};
        msg.len = data_len - offset;
        if (msg.len > sizeof(msg.data)) msg.len = sizeof(msg.data);
        memcpy(msg.data, data + offset, msg.len);
        xQueueSend(s_usb_rx_queue, &msg, 0);
        offset += msg.len;
    }
    return true;
}

static void usb_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "CDC device disconnected");
        s_usb_connected = false;
        if (s_usb_event_group) {
            xEventGroupSetBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);
        }
        break;

    case CDC_ACM_HOST_ERROR:
        // Ignore: CDC_ACM_HOST_ERROR is fired spuriously from residual async
        // transfer callbacks. Real disconnection is reported via
        // CDC_ACM_HOST_DEVICE_DISCONNECTED.
        ESP_LOGD(TAG, "CDC error %d (ignored)", event->data.error);
        break;

    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "CDC serial state: 0x%02X", event->data.serial_state.val);
        break;

    default:
        break;
    }
}

// ==============================================================
// new_dev_cb: called from USB Host context when a device appears
// NOTE: Cannot open CDC device here; must signal vcp_task instead.
// ==============================================================

static void usb_new_dev_cb(usb_device_handle_t usb_dev)
{
    // VID/PID was already captured in usb_enum_filter_cb().
    // Wake up vcp_task; it will open the device.
    if (s_dev_present_sem) {
        xSemaphoreGive(s_dev_present_sem);
    }
}

// ==============================================================
// USB Host Library Task (daemon)
// ==============================================================

/**
 * @brief Enumeration filter callback
 *
 * Raspberry Pi g_serial gadget (loaded with use_acm=1, the default) exposes
 * its CDC-ACM function under configuration #2 ("CDC ACM config").
 * We identify the Pi g_serial gadget by its VID:PID (0x0525:0xa4a7) and
 * override bConfigurationValue to 2.
 */
#define RPI_G_SERIAL_VID  0x0525u
#define RPI_G_SERIAL_PID  0xa4a7u

static bool usb_enum_filter_cb(const usb_device_desc_t *dev_desc,
                               uint8_t *bConfigurationValue)
{
    uint16_t vid = dev_desc->idVendor;
    uint16_t pid = dev_desc->idProduct;

    s_dev_vid = vid;
    s_dev_pid = pid;

    if (vid == RPI_G_SERIAL_VID && pid == RPI_G_SERIAL_PID) {
        *bConfigurationValue = 2;
        ESP_LOGI(TAG, "enum_filter: Raspberry Pi g_serial detected (VID=%04x PID=%04x), selecting config #2",
                 vid, pid);
    } else {
        ESP_LOGD(TAG, "enum_filter: VID=%04x PID=%04x bNumConfigs=%d, using default config #%d",
                 vid, pid, dev_desc->bNumConfigurations, *bConfigurationValue);
    }
    return true;
}

static void usb_lib_task(void *arg)
{
    TaskHandle_t notify_target = (TaskHandle_t)arg;
    ESP_LOGI(TAG, "USB lib task started");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LOWMED,
        .enum_filter_cb = usb_enum_filter_cb,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB host installed");

    if (notify_target) {
        xTaskNotifyGive(notify_target);
    }

    // Run forever: never call usb_host_uninstall().
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "USB lib: no clients, freeing devices");
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB lib: all devices free, waiting for next connection");
        }
    }
}

// ==============================================================
// VCP Connection Task
// ==============================================================

static void vcp_task(void *arg)
{
    screen_log("[USB] Starting USB host...\n");
    ESP_LOGI(TAG, "Starting USB host lib task");

    xEventGroupClearBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);

    xTaskCreate(usb_lib_task, "usb_lib", 4096,
                xTaskGetCurrentTaskHandle(),
                USB_HOST_PRIORITY, NULL);

    // Wait for USB host to be installed (timeout 5s)
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    if (notified == 0) {
        screen_log("[USB] Host init timeout! Rebooting...\n");
        ESP_LOGE(TAG, "USB host init timeout, rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ---- Install CDC-ACM driver with new_dev_cb ----
    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority   = USB_CDC_PRIORITY,
        .xCoreID                = 0,
        .new_dev_cb             = usb_new_dev_cb,
    };
    esp_err_t err = cdc_acm_host_install(&driver_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        screen_log("[USB] CDC driver install failed!\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ---- Register VCP drivers (FTDI, CP210x, CH34x) ----
    VCP::register_driver<FT23x>();
    VCP::register_driver<CP210x>();
    VCP::register_driver<CH34x>();
    ESP_LOGI(TAG, "VCP drivers registered");

    // Notify main task that USB is ready
    if (s_main_task_handle) {
        xTaskNotifyGive(s_main_task_handle);
    }

    // ---- Connection loop ----
    while (1) {
        screen_log("[USB] Waiting for device...\n");
        ESP_LOGI(TAG, "Waiting for USB device...");

        BaseType_t sem_taken = xSemaphoreTake(s_dev_present_sem, pdMS_TO_TICKS(3000));
        if (sem_taken != pdTRUE) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        // Drain extra semaphore counts
        while (xSemaphoreTake(s_dev_present_sem, 0) == pdTRUE) {}

        xEventGroupClearBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);

        uint16_t dev_vid = s_dev_vid;
        uint16_t dev_pid = s_dev_pid;
        ESP_LOGI(TAG, "Opening device VID=%04x PID=%04x", dev_vid, dev_pid);
        (void)dev_pid;

        // Known VCP VIDs: FTDI=0x0403, CP210x=0x10C4, CH34x=0x1A86
        static const uint16_t VCP_VIDS[] = {0x0403u, 0x10C4u, 0x1A86u};
        bool is_known_vcp_vid = false;
        for (uint16_t vid : VCP_VIDS) {
            if (dev_vid == vid) { is_known_vcp_vid = true; break; }
        }

        CdcAcmDevice *dev = nullptr;
        bool is_vcp = false;

        if (is_known_vcp_vid) {
            cdc_acm_host_device_config_t vcp_cfg = {};
            vcp_cfg.connection_timeout_ms = 100;
            vcp_cfg.out_buffer_size       = 512;
            vcp_cfg.in_buffer_size        = 512;
            vcp_cfg.event_cb              = usb_event_cb;
            vcp_cfg.data_cb               = usb_rx_cb;
            vcp_cfg.user_arg              = NULL;

            CdcAcmDevice *vcp_dev = VCP::open(&vcp_cfg);
            if (vcp_dev != nullptr) {
                dev    = vcp_dev;
                is_vcp = true;
                ESP_LOGI(TAG, "VCP device opened (FTDI/CP210x/CH34x)");
            }
        } else {
            ESP_LOGI(TAG, "VID=%04x not a known VCP vendor, skipping VCP::open()", dev_vid);
        }

        if (dev == nullptr) {
            cdc_acm_host_open_config_t cdc_cfg = {};
            cdc_cfg.vid                   = CDC_HOST_ANY_VID;
            cdc_cfg.pid                   = CDC_HOST_ANY_PID;
            cdc_cfg.interface_idx         = 0;
            cdc_cfg.dev_addr              = CDC_HOST_ANY_DEV_ADDR;
            cdc_cfg.connection_timeout_ms = 1000;
            cdc_cfg.out_buffer_size       = 512;
            cdc_cfg.in_buffer_size        = 512;
            cdc_cfg.event_cb              = usb_event_cb;
            cdc_cfg.data_cb               = usb_rx_cb;
            cdc_cfg.user_arg              = NULL;

            CdcAcmDevice *cdc_dev = new CdcAcmDevice();
            err = cdc_dev->open(&cdc_cfg);
            if (err == ESP_OK) {
                dev = cdc_dev;
                ESP_LOGI(TAG, "Standard CDC-ACM device opened");
            } else {
                ESP_LOGE(TAG, "CDC-ACM open failed: %s", esp_err_to_name(err));
                delete cdc_dev;
            }
        }

        if (dev == nullptr) {
            screen_log("[USB] Open failed, retrying...\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (is_vcp) {
            cdc_acm_line_coding_t line_coding = {
                .dwDTERate   = s_baud_rate,
                .bCharFormat = 0,
                .bParityType = 0,
                .bDataBits   = 8,
            };
            err = dev->line_coding_set(&line_coding);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "line_coding_set: %s (ignored)", esp_err_to_name(err));
            }
            err = dev->set_control_line_state(true, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "set_control_line_state: %s (ignored)", esp_err_to_name(err));
            }
        }

        s_vcp_dev       = dev;
        s_usb_connected = true;
        screen_log("[USB] Connected! Baud:%"PRIu32" %s\n",
                   s_baud_rate, is_vcp ? "(VCP)" : "(CDC)");
        ESP_LOGI(TAG, "USB connected, baud=%"PRIu32" %s",
                 s_baud_rate, is_vcp ? "(VCP)" : "(CDC)");
        update_status_bar();

        xEventGroupWaitBits(s_usb_event_group,
                            USB_DEV_DISCONNECTED_BIT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        s_vcp_dev       = nullptr;
        s_usb_connected = false;
        delete dev;
        screen_log("[USB] Disconnected.\n");
        ESP_LOGI(TAG, "USB device closed");
        update_status_bar();

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ==============================================================
// Main Application
// ==============================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== TAB5 Serial Terminal ===");

    s_main_task_handle = xTaskGetCurrentTaskHandle();

    // ---- Board init ----
    m5::tab5::m5tab5_component_config_t board_cfg = {};
    esp_err_t ret = s_tab5_board.begin(board_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(ret));
        return;
    }

    s_tab5_board.usb5v_enable(true);
    ESP_LOGI(TAG, "USB-A 5V power enabled");

    // ---- LCD/LVGL init ----
    ret = app_lcd_lvgl_init(s_tab5_board);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD/LVGL init failed: %s", esp_err_to_name(ret));
        return;
    }
    ui_create();
    term_clear_all();

    // Initial welcome message (processed through VT100 parser)
    const char *welcome =
        "\033[2J\033[H"                     // clear screen, home
        "\033[1;32mM5Stack TAB5 Serial Terminal\033[0m\n"
        "\033[32m============================\033[0m\n"
        "Initializing USB host...\n";
    for (const char *p = welcome; *p; p++) {
        vt100_process_byte((uint8_t)*p);
    }
    term_refresh_display();

    // ---- Queues, semaphores, and event groups ----
    s_key_queue        = xQueueCreate(32, sizeof(key_event_msg_t));
    s_usb_rx_queue     = xQueueCreate(64, sizeof(usb_rx_msg_t));
    s_screen_log_queue = xQueueCreate(32, sizeof(screen_log_msg_t));
    s_usb_event_group  = xEventGroupCreate();
    s_dev_present_sem  = xSemaphoreCreateCounting(8, 0);

    // ---- VCP task ----
    xTaskCreate(vcp_task, "vcp_task", 8192, NULL, USB_VCP_PRIORITY, NULL);

    // Wait for USB host to be ready
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "USB ready, starting main loop");

    // Print ready message through VT100 parser
    const char *ready_msg = "Connect a USB-serial device to the USB-A port.\n\n";
    for (const char *p = ready_msg; *p; p++) {
        vt100_process_byte((uint8_t)*p);
    }
    term_refresh_display();

    // ---- Keyboard init ----
    m5_tab5_kb_err_t kb_err = s_keyboard.begin(
        I2C_NUM_1,
        M5_TAB5_KB_DEFAULT_ADDR,
        M5_TAB5_KB_DEFAULT_SDA,
        M5_TAB5_KB_DEFAULT_SCL,
        M5_TAB5_KB_I2C_FREQ_400K,
        M5_TAB5_KB_DEFAULT_INT,
        M5_TAB5_KB_INT_MODE_HARDWARE
    );
    if (kb_err == M5_TAB5_KB_OK) {
        uint8_t version = 0;
        s_keyboard.getVersion(&version);
        ESP_LOGI(TAG, "Keyboard OK, FW: 0x%02X", version);
        s_keyboard.enableStringMode(keyboard_event_cb, NULL);
    } else {
        ESP_LOGW(TAG, "Keyboard not detected (err=%d)", kb_err);
        const char *warn = "\033[1;33m[WARNING] Keyboard not detected!\033[0m\n";
        for (const char *p = warn; *p; p++) vt100_process_byte((uint8_t)*p);
        term_refresh_display();
    }

    update_status_bar();

    // ---- Main loop ----
    ESP_LOGI(TAG, "Entering main loop");

    while (1) {
        // Process screen log messages from other tasks
        screen_log_msg_t log_msg;
        bool need_refresh = false;
        while (xQueueReceive(s_screen_log_queue, &log_msg, 0) == pdTRUE) {
            for (const char *p = log_msg.msg; *p; p++) {
                vt100_process_byte((uint8_t)*p);
            }
            need_refresh = true;
        }

        // Process USB RX data through VT100 parser
        usb_rx_msg_t rx_msg;
        while (xQueueReceive(s_usb_rx_queue, &rx_msg, 0) == pdTRUE) {
            for (size_t i = 0; i < rx_msg.len; i++) {
                vt100_process_byte(rx_msg.data[i]);
            }
            need_refresh = true;
        }

        if (need_refresh) {
            term_refresh_display();
            update_status_bar();
        }

        // Process keyboard input
        key_event_msg_t key_msg;
        if (xQueueReceive(s_key_queue, &key_msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            bool ctrl = (key_msg.modifier & 0x01) != 0;

            if (ctrl) {
                char k = key_msg.str[0];
                if (k == 'c' || k == 'C') {
                    // Ctrl+C: clear screen
                    term_clear_all();
                    const char *msg = "\033[1;32m[Screen cleared]\033[0m\n";
                    for (const char *p = msg; *p; p++) vt100_process_byte((uint8_t)*p);
                    term_refresh_display();
                    continue;
                }
                if (k == 'l' || k == 'L') {
                    term_refresh_display();
                    continue;
                }
                if (k == 'b' || k == 'B') {
                    static const uint32_t baud_rates[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
                    static int baud_idx = 4;
                    baud_idx = (baud_idx + 1) % (sizeof(baud_rates) / sizeof(baud_rates[0]));
                    s_baud_rate = baud_rates[baud_idx];
                    if (s_usb_connected && s_vcp_dev) {
                        cdc_acm_line_coding_t lc = {
                            .dwDTERate   = s_baud_rate,
                            .bCharFormat = 0,
                            .bParityType = 0,
                            .bDataBits   = 8,
                        };
                        s_vcp_dev->line_coding_set(&lc);
                    }
                    char msg[64];
                    snprintf(msg, sizeof(msg), "\n\033[1;33m[Baud: %"PRIu32"]\033[0m\n", s_baud_rate);
                    for (const char *p = msg; *p; p++) vt100_process_byte((uint8_t)*p);
                    term_refresh_display();
                    update_status_bar();
                    continue;
                }
                // Other Ctrl+key: send as control character to USB
                if (s_usb_connected && s_vcp_dev && k >= '@' && k <= '_') {
                    uint8_t ctrl_char = (uint8_t)(k - '@');
                    s_vcp_dev->tx_blocking(&ctrl_char, 1, 1000);
                }
                continue;
            }

            // TAB5 keyboard STRING mode: map special key names to VT100 sequences
            // and send to USB; no local echo (remote device echoes back).
            struct {
                const char *name;
                const char *seq;
            } special_keys[] = {
                { "enter",     "\r"         },
                { "backspace", "\x7f"       },  // DEL (most terminals expect 0x7F)
                { "tab",       "\t"         },
                { "up",        "\x1b[A"     },
                { "down",      "\x1b[B"     },
                { "right",     "\x1b[C"     },
                { "left",      "\x1b[D"     },
                { "home",      "\x1b[H"     },
                { "end",       "\x1b[F"     },
                { "pageup",    "\x1b[5~"    },
                { "pagedown",  "\x1b[6~"    },
                { "insert",    "\x1b[2~"    },
                { "delete",    "\x1b[3~"    },
                { "f1",        "\x1bOP"     },
                { "f2",        "\x1bOQ"     },
                { "f3",        "\x1bOR"     },
                { "f4",        "\x1bOS"     },
                { "f5",        "\x1b[15~"   },
                { "f6",        "\x1b[17~"   },
                { "f7",        "\x1b[18~"   },
                { "f8",        "\x1b[19~"   },
                { "f9",        "\x1b[20~"   },
                { "f10",       "\x1b[21~"   },
                { "f11",       "\x1b[23~"   },
                { "f12",       "\x1b[24~"   },
                { "escape",    "\x1b"       },
                { NULL, NULL }
            };

            bool handled = false;
            for (int k = 0; special_keys[k].name != NULL; k++) {
                if (strcasecmp(key_msg.str, special_keys[k].name) == 0) {
                    if (s_usb_connected && s_vcp_dev) {
                        const char *seq = special_keys[k].seq;
                        s_vcp_dev->tx_blocking((const uint8_t *)seq, strlen(seq), 1000);
                    }
                    handled = true;
                    break;
                }
            }

            if (!handled) {
                // Normal printable characters: send to USB, no local echo
                for (int i = 0; key_msg.str[i] != '\0'; i++) {
                    char c = key_msg.str[i];
                    if (s_usb_connected && s_vcp_dev) {
                        s_vcp_dev->tx_blocking((uint8_t *)&c, 1, 1000);
                    }
                    // No local echo: remote device will echo back via USB RX
                }
            }
            term_refresh_display();
        }
    }
}

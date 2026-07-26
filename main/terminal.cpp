/*
 * terminal.cpp — Terminal buffer management and VT100 escape sequence parser.
 *
 * SPDX-License-Identifier: MIT
 */

#include "terminal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <esp_log.h>

static const char *TAG = "terminal";

// ==============================================================
// SGR Color Palette
// ==============================================================

const lv_color_t TERM_COLORS[16] = {
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

// ==============================================================
// Terminal State
// ==============================================================

TermCell term_buffer[TERM_ROWS][TERM_COLS];

int  cursor_row         = 0;
int  cursor_col         = 0;
bool cursor_visible     = true;
bool cursor_blink_state = true;
bool row_dirty[TERM_ROWS] = {};

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

// Pending wrap (deferred wrap / VT100 "last column" flag):
// Set when a character is written to the last column (TERM_COLS-1).
// The actual line wrap is deferred until the next printable character arrives.
// Cleared by CR, LF, cursor-movement ESC sequences, and vt_clamp_cursor().
static bool pending_wrap = false;

// TX callback for DSR/DA responses (set by vt100_set_tx_cb)
static vt100_tx_cb_t s_tx_cb = nullptr;

// ==============================================================
// Terminal Buffer Operations
// ==============================================================

static void term_cell_clear(TermCell *cell)
{
    cell->ch   = ' ';
    cell->fg   = DEFAULT_FG;
    cell->bg   = DEFAULT_BG;
    cell->bold = 0;
}

void term_mark_dirty(int row)
{
    if (row >= 0 && row < TERM_ROWS) row_dirty[row] = true;
}

void term_mark_all_dirty(void)
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

void term_clear_all(void)
{
    term_clear_region(0, 0, TERM_ROWS - 1, TERM_COLS - 1);
    cursor_row   = 0;
    cursor_col   = 0;
    pending_wrap = false;
    scroll_top   = 0;
    scroll_bot   = TERM_ROWS - 1;
    cur_fg       = DEFAULT_FG;
    cur_bg       = DEFAULT_BG;
    cur_bold     = 0;
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

static void term_put_char_raw(char c)
{
    if (cursor_row < 0 || cursor_row >= TERM_ROWS) return;
    // Deferred wrap: if previous char filled the last column, wrap now before writing.
    if (pending_wrap) {
        pending_wrap = false;
        cursor_col = 0;
        if (cursor_row == scroll_bot) {
            term_scroll_up(1);
        } else {
            cursor_row++;
            if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1;
        }
    }
    if (cursor_col < 0 || cursor_col >= TERM_COLS) return;
    term_buffer[cursor_row][cursor_col].ch   = c;
    term_buffer[cursor_row][cursor_col].fg   = cur_bold ? (cur_fg | 8) : cur_fg;
    term_buffer[cursor_row][cursor_col].bg   = cur_bg;
    term_buffer[cursor_row][cursor_col].bold = cur_bold;
    term_mark_dirty(cursor_row);
    cursor_col++;
    if (cursor_col >= TERM_COLS) {
        // Reached last column: set pending wrap flag instead of wrapping immediately.
        cursor_col = TERM_COLS - 1;
        pending_wrap = true;
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
static vt_state_t vt_state       = VT_STATE_NORMAL;
static int        vt_params[VT_MAX_PARAMS];
static int        vt_num_params  = 0;
static bool       vt_param_started = false;

static void vt_reset_params(void)
{
    for (int i = 0; i < VT_MAX_PARAMS; i++) vt_params[i] = -1;
    vt_num_params    = 0;
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
    pending_wrap = false;
}

// Map 256-color index to nearest 16-color index
static uint8_t color256_to_16(int idx)
{
    if (idx < 0)   idx = 0;
    if (idx > 255) idx = 255;
    if (idx < 16)  return (uint8_t)idx;
    if (idx >= 232) {
        int v = idx - 232;
        if (v < 6)  return 0;
        if (v < 12) return 8;
        if (v < 18) return 7;
        return 15;
    }
    int cube = idx - 16;
    int b = cube % 6;
    int g = (cube / 6) % 6;
    int r = cube / 36;
    int rb = (r >= 3) ? 1 : 0;
    int gb = (g >= 3) ? 1 : 0;
    int bb = (b >= 3) ? 1 : 0;
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
            int mode = vt_param(i + 1, -1);
            if (mode == 5 && i + 2 < n) {
                cur_fg = color256_to_16(vt_param(i + 2, 0));
                i += 2;
            } else if (mode == 2 && i + 4 < n) {
                int r = vt_param(i + 2, 0);
                int g = vt_param(i + 3, 0);
                int b = vt_param(i + 4, 0);
                bool bright = (r > 170 || g > 170 || b > 170);
                uint8_t base = (uint8_t)((r > 85 ? 1 : 0) | (g > 85 ? 2 : 0) | (b > 85 ? 4 : 0));
                cur_fg = bright ? (base | 8) : base;
                i += 4;
            }
        } else if (p == 48) {
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
    ESP_LOGD(TAG, "CSI final='%c' row=%d col=%d pw=%d", final_ch, cursor_row, cursor_col, (int)pending_wrap);

    switch (final_ch) {
    case 'A': { int n = vt_param(0, 1); if (n < 1) n = 1; cursor_row -= n; vt_clamp_cursor(); break; }
    case 'B': { int n = vt_param(0, 1); if (n < 1) n = 1; cursor_row += n; vt_clamp_cursor(); break; }
    case 'C': { int n = vt_param(0, 1); if (n < 1) n = 1; cursor_col += n; vt_clamp_cursor(); break; }
    case 'D': { int n = vt_param(0, 1); if (n < 1) n = 1; cursor_col -= n; vt_clamp_cursor(); break; }
    case 'E': { int n = vt_param(0, 1); if (n < 1) n = 1; cursor_row += n; cursor_col = 0; vt_clamp_cursor(); break; }
    case 'F': { int n = vt_param(0, 1); if (n < 1) n = 1; cursor_row -= n; cursor_col = 0; vt_clamp_cursor(); break; }
    case 'G': { int n = vt_param(0, 1); if (n < 1) n = 1; cursor_col = n - 1; vt_clamp_cursor(); break; }
    case 'H':
    case 'f': {
        int row = vt_param(0, 1); if (row < 1) row = 1;
        int col = vt_param(1, 1); if (col < 1) col = 1;
        cursor_row = row - 1; cursor_col = col - 1;
        vt_clamp_cursor();
        break;
    }
    case 'd': { int n = vt_param(0, 1); if (n < 1) n = 1; cursor_row = n - 1; vt_clamp_cursor(); break; }
    case 's': { saved_row = cursor_row; saved_col = cursor_col; pending_wrap = false; break; }
    case 'u': { cursor_row = saved_row; cursor_col = saved_col; vt_clamp_cursor(); break; }
    case 'J': {
        int n = vt_param(0, 0);
        if (n == 0)      term_clear_region(cursor_row, cursor_col, TERM_ROWS - 1, TERM_COLS - 1);
        else if (n == 1) term_clear_region(0, 0, cursor_row, cursor_col);
        else             term_clear_region(0, 0, TERM_ROWS - 1, TERM_COLS - 1);
        break;
    }
    case 'K': {
        int n = vt_param(0, 0);
        if (n == 0)      for (int c = cursor_col; c < TERM_COLS; c++) term_cell_clear(&term_buffer[cursor_row][c]);
        else if (n == 1) for (int c = 0; c <= cursor_col; c++) term_cell_clear(&term_buffer[cursor_row][c]);
        else             for (int c = 0; c < TERM_COLS; c++) term_cell_clear(&term_buffer[cursor_row][c]);
        term_mark_dirty(cursor_row);
        break;
    }
    case 'S': { int n = vt_param(0, 1); if (n < 1) n = 1; term_scroll_up(n); break; }
    case 'T': { int n = vt_param(0, 1); if (n < 1) n = 1; term_scroll_down(n); break; }
    case 'r': {
        int top = vt_param(0, 1); if (top < 1) top = 1;
        int bot = vt_param(1, TERM_ROWS); if (bot < 1 || bot > TERM_ROWS) bot = TERM_ROWS;
        if (top < bot) {
            scroll_top = top - 1; scroll_bot = bot - 1;
            cursor_row = 0; cursor_col = 0; pending_wrap = false;
        }
        break;
    }
    case 'L': {
        int n = vt_param(0, 1); if (n < 1) n = 1;
        if (cursor_row >= scroll_top && cursor_row <= scroll_bot) {
            int old_top = scroll_top; scroll_top = cursor_row;
            term_scroll_down(n); scroll_top = old_top;
        }
        break;
    }
    case 'M': {
        int n = vt_param(0, 1); if (n < 1) n = 1;
        if (cursor_row >= scroll_top && cursor_row <= scroll_bot) {
            int old_top = scroll_top; scroll_top = cursor_row;
            term_scroll_up(n); scroll_top = old_top;
        }
        break;
    }
    case '@': {
        int n = vt_param(0, 1); if (n < 1) n = 1;
        if (cursor_row < TERM_ROWS) {
            for (int c = TERM_COLS - 1; c >= cursor_col + n; c--)
                term_buffer[cursor_row][c] = term_buffer[cursor_row][c - n];
            for (int c = cursor_col; c < cursor_col + n && c < TERM_COLS; c++)
                term_cell_clear(&term_buffer[cursor_row][c]);
            term_mark_dirty(cursor_row);
        }
        break;
    }
    case 'P': {
        int n = vt_param(0, 1); if (n < 1) n = 1;
        if (cursor_row < TERM_ROWS) {
            for (int c = cursor_col; c < TERM_COLS - n; c++)
                term_buffer[cursor_row][c] = term_buffer[cursor_row][c + n];
            for (int c = TERM_COLS - n; c < TERM_COLS; c++)
                term_cell_clear(&term_buffer[cursor_row][c]);
            term_mark_dirty(cursor_row);
        }
        break;
    }
    case 'X': {
        int n = vt_param(0, 1); if (n < 1) n = 1;
        for (int c = cursor_col; c < cursor_col + n && c < TERM_COLS; c++)
            term_cell_clear(&term_buffer[cursor_row][c]);
        term_mark_dirty(cursor_row);
        break;
    }
    case 'm': { vt_sgr(); break; }
    case 'n': {
        int param = vt_param(0, 0);
        if (param == 6) {
            char resp[32];
            int len = snprintf(resp, sizeof(resp), "\x1b[%d;%dR", cursor_row + 1, cursor_col + 1);
            ESP_LOGD(TAG, "DSR CPR: responding ESC[%d;%dR", cursor_row + 1, cursor_col + 1);
            if (s_tx_cb) s_tx_cb((const uint8_t *)resp, (size_t)len);
        } else if (param == 5) {
            const char *resp = "\x1b[0n";
            if (s_tx_cb) s_tx_cb((const uint8_t *)resp, 4);
        }
        break;
    }
    case 'c': {
        if (vt_param(0, 0) == 0) {
            const char *resp = "\x1b[?1;0c";
            if (s_tx_cb) s_tx_cb((const uint8_t *)resp, strlen(resp));
        }
        break;
    }
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
        if (n == 25) cursor_visible = true;
        // 1049: alternate screen buffer (ignore)
        // 1: application cursor keys (ignore)
        // 7: auto-wrap (always on, ignore)
    } else if (final_ch == 'l') {
        if (n == 25) cursor_visible = false;
    }
}

// ==============================================================
// VT100 TX callback registration
// ==============================================================

void vt100_set_tx_cb(vt100_tx_cb_t cb)
{
    s_tx_cb = cb;
}

// ==============================================================
// Main byte processor
// ==============================================================

void vt100_process_byte(uint8_t byte)
{
    char c = (char)byte;

    switch (vt_state) {
    case VT_STATE_NORMAL:
        if (c == 0x1B) {
            vt_state = VT_STATE_ESC;
        } else if (c == '\n') {
            pending_wrap = false;
            if (cursor_row == scroll_bot) {
                term_scroll_up(1);
            } else if (cursor_row < TERM_ROWS - 1) {
                cursor_row++;
            }
        } else if (c == '\r') {
            pending_wrap = false;
            cursor_col = 0;
        } else if (c == '\b' || c == 0x7F) {
            // BS/DEL: move cursor left only; does NOT erase character
            pending_wrap = false;
            if (cursor_col > 0) {
                cursor_col--;
                term_mark_dirty(cursor_row);
            }
        } else if (c == '\t') {
            pending_wrap = false;
            int next_tab = (cursor_col + 8) & ~7;
            cursor_col = (next_tab >= TERM_COLS) ? TERM_COLS - 1 : next_tab;
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
            saved_row = cursor_row; saved_col = cursor_col;
            pending_wrap = false;
            vt_state = VT_STATE_NORMAL;
        } else if (c == '8') {
            cursor_row = saved_row; cursor_col = saved_col;
            vt_clamp_cursor();
            vt_state = VT_STATE_NORMAL;
        } else if (c == 'c') {
            term_clear_all();
            vt_state = VT_STATE_NORMAL;
        } else if (c == 'D') {
            // Index: move cursor down, scroll if at bottom
            pending_wrap = false;
            if (cursor_row == scroll_bot) term_scroll_up(1);
            else { cursor_row++; if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1; }
            vt_state = VT_STATE_NORMAL;
        } else if (c == 'M') {
            // Reverse Index: move cursor up, scroll down if at top
            pending_wrap = false;
            if (cursor_row == scroll_top) term_scroll_down(1);
            else { cursor_row--; if (cursor_row < 0) cursor_row = 0; }
            vt_state = VT_STATE_NORMAL;
        } else if (c == 'E') {
            // Next Line
            pending_wrap = false;
            cursor_col = 0;
            if (cursor_row == scroll_bot) term_scroll_up(1);
            else { cursor_row++; if (cursor_row >= TERM_ROWS) cursor_row = TERM_ROWS - 1; }
            vt_state = VT_STATE_NORMAL;
        } else if (c == '#') {
            vt_state = VT_STATE_ESC_HASH;
        } else if (c == '(' || c == ')' || c == '*' || c == '+') {
            vt_state = VT_STATE_ESC_CHARSET;
        } else {
            vt_state = VT_STATE_NORMAL;
        }
        break;

    case VT_STATE_CSI:
        if (c == '?') {
            vt_state = VT_STATE_CSI_PRIV;
            vt_reset_params();
        } else if (c >= '0' && c <= '9') {
            if (!vt_param_started) {
                if (vt_num_params < VT_MAX_PARAMS) { vt_params[vt_num_params] = c - '0'; vt_num_params++; }
                vt_param_started = true;
            } else {
                int idx = vt_num_params - 1;
                if (idx < VT_MAX_PARAMS) {
                    if (vt_params[idx] < 0) vt_params[idx] = 0;
                    vt_params[idx] = vt_params[idx] * 10 + (c - '0');
                }
            }
        } else if (c == ';') {
            if (!vt_param_started && vt_num_params < VT_MAX_PARAMS) { vt_params[vt_num_params] = -1; vt_num_params++; }
            vt_param_started = false;
        } else if (c >= 0x40 && c <= 0x7E) {
            vt_process_csi(c);
            vt_state = VT_STATE_NORMAL;
        } else {
            vt_state = VT_STATE_NORMAL;
        }
        break;

    case VT_STATE_CSI_PRIV:
        if (c >= '0' && c <= '9') {
            if (!vt_param_started) {
                if (vt_num_params < VT_MAX_PARAMS) { vt_params[vt_num_params] = c - '0'; vt_num_params++; }
                vt_param_started = true;
            } else {
                int idx = vt_num_params - 1;
                if (idx < VT_MAX_PARAMS) {
                    if (vt_params[idx] < 0) vt_params[idx] = 0;
                    vt_params[idx] = vt_params[idx] * 10 + (c - '0');
                }
            }
        } else if (c == ';') {
            if (!vt_param_started && vt_num_params < VT_MAX_PARAMS) { vt_params[vt_num_params] = -1; vt_num_params++; }
            vt_param_started = false;
        } else if (c >= 0x40 && c <= 0x7E) {
            vt_process_csi_priv(c);
            vt_state = VT_STATE_NORMAL;
        } else {
            vt_state = VT_STATE_NORMAL;
        }
        break;

    case VT_STATE_ESC_HASH:
        if (c == '8') {
            // DECALN: fill screen with 'E' (alignment test)
            for (int r = 0; r < TERM_ROWS; r++)
                for (int col = 0; col < TERM_COLS; col++) {
                    term_buffer[r][col].ch = 'E'; term_buffer[r][col].fg = DEFAULT_FG;
                    term_buffer[r][col].bg = DEFAULT_BG; term_buffer[r][col].bold = 0;
                }
            term_mark_all_dirty();
        }
        vt_state = VT_STATE_NORMAL;
        break;

    case VT_STATE_ESC_CHARSET:
        // Consume charset designator byte; charset switching not implemented
        vt_state = VT_STATE_NORMAL;
        break;
    }
}

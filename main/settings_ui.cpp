/*
 * settings_ui.cpp — LVGL-based settings screen overlay.
 *
 * Layout (landscape 1280x720):
 *   ┌──────────────────────────────────────────────────────────┐
 *   │  TAB5 Serial Terminal — Settings          [Ctrl+Alt+S]   │  title bar 40px
 *   ├──────────────────────────────────────────────────────────┤
 *   │  Baud Rate:   [115200 ▼]                                 │  row 1
 *   │  Interface:   [USB Serial ▼]                             │  row 2
 *   │  Log Level:   [INFO ▼]                                   │  row 3
 *   ├──────────────────────────────────────────────────────────┤
 *   │  (i) PortA and Log Level changes take effect immediately  │  note
 *   │  (i) Baud rate is applied when Save & Close is pressed    │
 *   ├──────────────────────────────────────────────────────────┤
 *   │                    [ Save & Close ]                       │  button
 *   └──────────────────────────────────────────────────────────┘
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_ui.h"
#include "settings.h"
#include "terminal.h"    // LVGL_W, LVGL_H, STATUS_BAR_H
#include "display.h"     // term_refresh_display, update_status_bar

#include <esp_log.h>
#include <string.h>
#include "lvgl.h"
#include "lvgl_port.h"

static const char *TAG = "settings_ui";

// ==============================================================
// Internal state
// ==============================================================

static lv_obj_t *s_overlay       = NULL;  // root overlay panel
static lv_obj_t *s_dd_baud       = NULL;  // baud rate dropdown
static lv_obj_t *s_dd_iface      = NULL;  // interface dropdown
static lv_obj_t *s_dd_log        = NULL;  // log level dropdown

// Callback invoked when settings are saved (set by main.cpp)
typedef void (*settings_saved_cb_t)(const app_settings_t *s);
static settings_saved_cb_t s_saved_cb = NULL;

// Current settings snapshot (filled at open time)
static app_settings_t s_current = {};

// ==============================================================
// Baud rate table
// ==============================================================

static const uint32_t BAUD_TABLE[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
};
static const int BAUD_TABLE_LEN = (int)(sizeof(BAUD_TABLE) / sizeof(BAUD_TABLE[0]));

static const char *BAUD_OPTIONS =
    "9600\n19200\n38400\n57600\n115200\n230400\n460800\n921600";

static int baud_to_index(uint32_t baud)
{
    for (int i = 0; i < BAUD_TABLE_LEN; i++) {
        if (BAUD_TABLE[i] == baud) return i;
    }
    return 4; // default: 115200
}

// ==============================================================
// Helper: create a labeled row with a dropdown
// ==============================================================

static lv_obj_t *create_row(lv_obj_t *parent, int y_pos,
                             const char *label_text,
                             const char *options,
                             int selected_idx)
{
    // Label
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_pos(lbl, 40, y_pos + 8);

    // Dropdown
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, (uint16_t)selected_idx);
    // Taller height (56px) for easier touch on Tab5 touchscreen
    lv_obj_set_size(dd, 500, 56);
    lv_obj_set_pos(dd, 300, y_pos);
    lv_obj_add_flag(dd, LV_OBJ_FLAG_CLICKABLE);  // ensure hit-test works in LVGL v9
    lv_obj_set_style_text_font(dd, &lv_font_unscii_16, 0);
    lv_obj_set_style_bg_color(dd, lv_color_make(40, 40, 60), 0);
    lv_obj_set_style_text_color(dd, lv_color_white(), 0);
    lv_obj_set_style_border_color(dd, lv_color_make(100, 100, 180), 0);
    lv_obj_set_style_border_width(dd, 2, 0);
    // Extend touch hit area by 10px on all sides
    lv_obj_set_ext_click_area(dd, 10);

    // Style the dropdown list
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_bg_color(list, lv_color_make(30, 30, 50), 0);
        lv_obj_set_style_text_color(list, lv_color_white(), 0);
        lv_obj_set_style_text_font(list, &lv_font_unscii_16, 0);
        // Larger row height for easier touch selection
        lv_obj_set_style_pad_top(list, 8, LV_PART_ITEMS);
        lv_obj_set_style_pad_bottom(list, 8, LV_PART_ITEMS);
    }

    return dd;
}

// ==============================================================
// Save & Close button callback
// ==============================================================

static void save_close_cb(lv_event_t *e)
{
    (void)e;

    // Read values from dropdowns
    app_settings_t ns = s_current;

    uint16_t baud_idx = lv_dropdown_get_selected(s_dd_baud);
    if (baud_idx < (uint16_t)BAUD_TABLE_LEN) {
        ns.baud_rate = BAUD_TABLE[baud_idx];
    }

    uint16_t iface_idx = lv_dropdown_get_selected(s_dd_iface);
    ns.serial_if = (serial_if_t)iface_idx;

    uint16_t log_idx = lv_dropdown_get_selected(s_dd_log);
    ns.log_level = (app_log_level_t)log_idx;

    // Save to NVS
    settings_save(&ns);

    // Apply immediately
    settings_apply(&ns);

    // Update snapshot
    s_current = ns;

    ESP_LOGI(TAG, "Settings saved: baud=%"PRIu32" iface=%d log=%d",
             ns.baud_rate, (int)ns.serial_if, (int)ns.log_level);

    // Close the overlay
    settings_ui_close();
}

// ==============================================================
// Public API
// ==============================================================

void settings_ui_open(const app_settings_t *current)
{
    if (s_overlay != NULL) {
        // Already open — just bring to front
        lvgl_port_lock(0);
        lv_obj_move_foreground(s_overlay);
        lvgl_port_unlock();
        return;
    }

    s_current = *current;

    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();

    // ---- Root overlay panel ----
    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, LVGL_W, LVGL_H - STATUS_BAR_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_make(20, 20, 35), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Title bar ----
    lv_obj_t *title_bar = lv_obj_create(s_overlay);
    lv_obj_set_size(title_bar, LVGL_W, 48);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_make(0, 80, 160), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(title_bar);
    lv_label_set_text(title_lbl, "  TAB5 Serial Terminal  -  Settings  (Ctrl+Alt+S to close)");
    lv_obj_set_style_text_font(title_lbl, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_white(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // ---- Separator ----
    lv_obj_t *sep = lv_obj_create(s_overlay);
    lv_obj_set_size(sep, LVGL_W, 2);
    lv_obj_set_pos(sep, 0, 48);
    lv_obj_set_style_bg_color(sep, lv_color_make(80, 80, 120), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    // ---- Setting rows ----
    // Row 1: Baud Rate
    s_dd_baud = create_row(s_overlay, 70,
                           "Baud Rate:",
                           BAUD_OPTIONS,
                           baud_to_index(current->baud_rate));

    // Row 2: Interface
    s_dd_iface = create_row(s_overlay, 130,
                            "Interface:",
                            "USB Serial\nPortA UART (future)",
                            (int)current->serial_if);

    // Row 3: Log Level
    s_dd_log = create_row(s_overlay, 190,
                          "Log Level:",
                          "NONE\nERROR\nWARN\nINFO\nDEBUG\nVERBOSE",
                          (int)current->log_level);

    // ---- Note ----
    lv_obj_t *note = lv_label_create(s_overlay);
    lv_label_set_text(note,
        "  Note: Log level takes effect immediately.\n"
        "  Baud rate and interface are applied when Save & Close is pressed.\n"
        "  PortA interface is not yet implemented.");
    lv_obj_set_style_text_font(note, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(note, lv_color_make(180, 180, 180), 0);
    lv_obj_set_pos(note, 40, 260);
    lv_obj_set_width(note, LVGL_W - 80);

    // ---- Separator 2 ----
    lv_obj_t *sep2 = lv_obj_create(s_overlay);
    lv_obj_set_size(sep2, LVGL_W, 2);
    lv_obj_set_pos(sep2, 0, 360);
    lv_obj_set_style_bg_color(sep2, lv_color_make(80, 80, 120), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);

    // ---- Save & Close button ----
    // Use lv_button_create (LVGL v9 API); lv_btn_create is v8 and may fall back
    // to lv_obj_create which lacks LV_OBJ_FLAG_CLICKABLE by default.
    lv_obj_t *btn = lv_button_create(s_overlay);
    lv_obj_set_size(btn, 480, 72);
    // Use absolute position instead of lv_obj_align to avoid layout timing issues
    // LVGL_W=1280, btn_w=480 -> x=(1280-480)/2=400
    // overlay_h=700, btn_h=72, margin=30 -> y=700-72-30=598
    lv_obj_set_pos(btn, (LVGL_W - 480) / 2, (LVGL_H - STATUS_BAR_H) - 72 - 30);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn, lv_color_make(0, 140, 60), 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0, 180, 80), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_ext_click_area(btn, 16);
    lv_obj_add_event_cb(btn, save_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Save & Close");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_center(btn_lbl);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Settings UI opened");
}

void settings_ui_close(void)
{
    if (s_overlay == NULL) return;

    lvgl_port_lock(0);
    lv_obj_delete(s_overlay);
    s_overlay  = NULL;
    s_dd_baud  = NULL;
    s_dd_iface = NULL;
    s_dd_log   = NULL;
    lvgl_port_unlock();

    // Force full terminal redraw so the screen is restored
    term_mark_all_dirty();
    term_refresh_display();
    update_status_bar();

    ESP_LOGI(TAG, "Settings UI closed");
}

bool settings_ui_is_open(void)
{
    return s_overlay != NULL;
}

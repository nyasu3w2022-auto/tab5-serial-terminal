/*
 * settings_ui.cpp — LVGL-based settings screen overlay.
 *
 * Settings values open a fixed, application-owned selection panel.  This
 * preserves one-tap list selection without using the LVGL dropdown widget's
 * screen-level popup list.  The panel, scrim, and every option button are
 * created as children of the settings overlay when it opens; selecting a
 * value only changes visibility and never reparents, auto-sizes, or scrolls
 * an LVGL list object.
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_ui.h"
#include "settings.h"
#include "terminal.h"    // LVGL_W, LVGL_H, STATUS_BAR_H
#include "display.h"     // term_refresh_display, update_status_bar

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_log.h>
#include "lvgl.h"
#include "lvgl_port.h"

static const char *TAG = "settings_ui";

// ==============================================================
// Internal state and fixed option tables
// ==============================================================

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_picker_scrim = NULL;
static lv_obj_t *s_picker_panel = NULL;
static lv_obj_t *s_picker_title = NULL;
static lv_obj_t *s_picker_hint = NULL;
static settings_saved_cb_t s_saved_cb = NULL;
static app_settings_t s_current = {};

static const uint32_t BAUD_TABLE[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
};
static constexpr size_t BAUD_TABLE_LEN = sizeof(BAUD_TABLE) / sizeof(BAUD_TABLE[0]);
static constexpr size_t MAX_PICKER_OPTIONS = BAUD_TABLE_LEN;

static const char *const BAUD_LABELS[] = {
    "9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"
};
static const char *const IFACE_LABELS[] = {
    "USB Serial", "PortA UART (GPIO53/54)", "MBUS UART2 (GPIO6/7)"
};
static const char *const LOG_LABELS[] = {
    "NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"
};
static const char *const FONT_LABELS[] = {
    "Small (160x43)", "Large (91x25)"
};
static const char *const ECHO_LABELS[] = {
    "OFF", "ON"
};
static const char *const INPUT_MODE_LABELS[] = {
    "Direct", "Japanese (SKK)"
};
static const char *const PUNCTUATION_LABELS[] = {
    "Japanese (JP)", "ASCII", "Fullwidth"
};
static const char *const LEARNING_LABELS[] = {
    "Off (no learning)", "Deferred (batch)", "Manual save"
};

enum class choice_id_t : uint8_t {
    BAUD = 0,
    INTERFACE,
    LOG_LEVEL,
    FONT_SIZE,
    ECHO_BACK,
    INPUT_MODE,
    PUNCTUATION,
    LEARNING,
    COUNT,
};

struct choice_control_t {
    lv_obj_t *button = NULL;
    lv_obj_t *value_label = NULL;
    choice_id_t id = choice_id_t::BAUD;
    uint8_t selected = 0;
};

struct picker_option_t {
    choice_control_t *control = NULL;
    uint8_t option_index = 0;
    lv_obj_t *button = NULL;
    lv_obj_t *label = NULL;
};

static choice_control_t s_choices[(size_t)choice_id_t::COUNT] = {};
static picker_option_t s_picker_options[MAX_PICKER_OPTIONS] = {};
static choice_control_t *s_picker_active = NULL;

static constexpr int PICKER_PANEL_W = 1040;
static constexpr int PICKER_PANEL_H = 520;
static constexpr int PICKER_PANEL_X = (LVGL_W - PICKER_PANEL_W) / 2;
static constexpr int PICKER_PANEL_Y = ((LVGL_H - STATUS_BAR_H) - PICKER_PANEL_H) / 2;
static constexpr int PICKER_OPTION_H = 60;
static constexpr int PICKER_OPTION_GAP_X = 30;
static constexpr int PICKER_OPTION_GAP_Y = 14;

// ==============================================================
// Setting value helpers
// ==============================================================

static size_t choice_index(choice_id_t id)
{
    return (size_t)id;
}

static const char *choice_name(choice_id_t id)
{
    switch (id) {
    case choice_id_t::BAUD:        return "Baud Rate";
    case choice_id_t::INTERFACE:   return "Interface";
    case choice_id_t::LOG_LEVEL:   return "Log Level";
    case choice_id_t::FONT_SIZE:   return "Font Size";
    case choice_id_t::ECHO_BACK:   return "Echo Back";
    case choice_id_t::INPUT_MODE:  return "Input Mode";
    case choice_id_t::PUNCTUATION: return "Punctuation";
    case choice_id_t::LEARNING:    return "Learning";
    case choice_id_t::COUNT:       return "";
    }
    return "";
}

static size_t choice_count(choice_id_t id)
{
    switch (id) {
    case choice_id_t::BAUD:        return BAUD_TABLE_LEN;
    case choice_id_t::INTERFACE:   return sizeof(IFACE_LABELS) / sizeof(IFACE_LABELS[0]);
    case choice_id_t::LOG_LEVEL:   return sizeof(LOG_LABELS) / sizeof(LOG_LABELS[0]);
    case choice_id_t::FONT_SIZE:   return sizeof(FONT_LABELS) / sizeof(FONT_LABELS[0]);
    case choice_id_t::ECHO_BACK:   return sizeof(ECHO_LABELS) / sizeof(ECHO_LABELS[0]);
    case choice_id_t::INPUT_MODE:  return sizeof(INPUT_MODE_LABELS) / sizeof(INPUT_MODE_LABELS[0]);
    case choice_id_t::PUNCTUATION: return sizeof(PUNCTUATION_LABELS) / sizeof(PUNCTUATION_LABELS[0]);
    case choice_id_t::LEARNING:    return sizeof(LEARNING_LABELS) / sizeof(LEARNING_LABELS[0]);
    case choice_id_t::COUNT:       return 0;
    }
    return 0;
}

static const char *choice_label(choice_id_t id, size_t selected)
{
    switch (id) {
    case choice_id_t::BAUD:        return BAUD_LABELS[selected];
    case choice_id_t::INTERFACE:   return IFACE_LABELS[selected];
    case choice_id_t::LOG_LEVEL:   return LOG_LABELS[selected];
    case choice_id_t::FONT_SIZE:   return FONT_LABELS[selected];
    case choice_id_t::ECHO_BACK:   return ECHO_LABELS[selected];
    case choice_id_t::INPUT_MODE:  return INPUT_MODE_LABELS[selected];
    case choice_id_t::PUNCTUATION: return PUNCTUATION_LABELS[selected];
    case choice_id_t::LEARNING:    return LEARNING_LABELS[selected];
    case choice_id_t::COUNT:       return "";
    }
    return "";
}

static uint8_t baud_to_index(uint32_t baud)
{
    for (size_t i = 0; i < BAUD_TABLE_LEN; ++i) {
        if (BAUD_TABLE[i] == baud) return (uint8_t)i;
    }
    return 4;  // 115200
}

static uint8_t validated_index(int value, size_t count, uint8_t fallback)
{
    return (value >= 0 && (size_t)value < count) ? (uint8_t)value : fallback;
}

static void apply_choice_to_snapshot(const choice_control_t &control)
{
    switch (control.id) {
    case choice_id_t::BAUD:
        s_current.baud_rate = BAUD_TABLE[control.selected];
        break;
    case choice_id_t::INTERFACE:
        s_current.serial_if = (serial_if_t)control.selected;
        break;
    case choice_id_t::LOG_LEVEL:
        s_current.log_level = (app_log_level_t)control.selected;
        break;
    case choice_id_t::FONT_SIZE:
        s_current.font_size = (app_font_size_t)control.selected;
        break;
    case choice_id_t::ECHO_BACK:
        s_current.local_echo = (local_echo_t)control.selected;
        break;
    case choice_id_t::INPUT_MODE:
        s_current.input_mode = (app_input_mode_t)control.selected;
        break;
    case choice_id_t::PUNCTUATION:
        s_current.punctuation_style = (app_punctuation_style_t)control.selected;
        break;
    case choice_id_t::LEARNING:
        s_current.learning_save_mode = (app_learning_save_mode_t)control.selected;
        break;
    case choice_id_t::COUNT:
        break;
    }
}

static void update_choice_label(choice_control_t *control)
{
    if (control == NULL || control->value_label == NULL) return;
    lv_label_set_text(control->value_label, choice_label(control->id, control->selected));
    lv_obj_center(control->value_label);
}

// ==============================================================
// Fixed selection panel
// ==============================================================

static void hide_picker(void)
{
    if (s_picker_scrim != NULL) lv_obj_add_flag(s_picker_scrim, LV_OBJ_FLAG_HIDDEN);
    if (s_picker_panel != NULL) lv_obj_add_flag(s_picker_panel, LV_OBJ_FLAG_HIDDEN);
    s_picker_active = NULL;
}

static void picker_cancel_cb(lv_event_t *event)
{
    (void)event;
    hide_picker();
}

static void picker_option_cb(lv_event_t *event)
{
    picker_option_t *option = (picker_option_t *)lv_event_get_user_data(event);
    if (option == NULL || option->control == NULL || option->control != s_picker_active) return;

    option->control->selected = option->option_index;
    apply_choice_to_snapshot(*option->control);
    update_choice_label(option->control);
    ESP_LOGI(TAG, "%s selected: %s", choice_name(option->control->id),
             choice_label(option->control->id, option->control->selected));
    hide_picker();
}

static void show_picker(choice_control_t *control)
{
    if (control == NULL || s_picker_panel == NULL || s_picker_scrim == NULL) return;

    const size_t count = choice_count(control->id);
    if (count == 0 || count > MAX_PICKER_OPTIONS) return;

    s_picker_active = control;
    lv_label_set_text_fmt(s_picker_title, "Select %s", choice_name(control->id));
    lv_label_set_text(s_picker_hint, "Tap a value to select it. Cancel keeps the current choice.");

    // Eight baud-rate choices use a 2 x 4 grid. All shorter lists use a
    // single column, which preserves a large, easy-to-read touch target.
    const int columns = count >= 4 ? 2 : 1;
    const int rows = (int)((count + (size_t)columns - 1) / (size_t)columns);
    const int content_w = PICKER_PANEL_W - 80;
    const int option_w = (content_w - (columns - 1) * PICKER_OPTION_GAP_X) / columns;
    const int grid_w = columns * option_w + (columns - 1) * PICKER_OPTION_GAP_X;
    const int grid_x = (PICKER_PANEL_W - grid_w) / 2;
    const int grid_h = rows * PICKER_OPTION_H + (rows - 1) * PICKER_OPTION_GAP_Y;
    const int grid_y = 118 + (270 - grid_h) / 2;

    for (size_t i = 0; i < MAX_PICKER_OPTIONS; ++i) {
        picker_option_t *option = &s_picker_options[i];
        if (i >= count) {
            lv_obj_add_flag(option->button, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        option->control = control;
        option->option_index = (uint8_t)i;
        lv_label_set_text(option->label, choice_label(control->id, i));
        lv_obj_center(option->label);

        const int row = (int)(i / (size_t)columns);
        const int col = (int)(i % (size_t)columns);
        lv_obj_set_size(option->button, option_w, PICKER_OPTION_H);
        lv_obj_set_pos(option->button,
                       grid_x + col * (option_w + PICKER_OPTION_GAP_X),
                       grid_y + row * (PICKER_OPTION_H + PICKER_OPTION_GAP_Y));
        lv_obj_clear_flag(option->button, LV_OBJ_FLAG_HIDDEN);

        const bool selected = i == (size_t)control->selected;
        lv_obj_set_style_bg_color(option->button,
                                  selected ? lv_color_make(0, 115, 180)
                                           : lv_color_make(45, 45, 72),
                                  0);
        lv_obj_set_style_border_color(option->button,
                                      selected ? lv_color_make(130, 220, 255)
                                               : lv_color_make(110, 110, 175),
                                      0);
    }

    // Both objects are persistent children of s_overlay. Their order is
    // stable: scrim blocks background controls, panel is moved above it.
    lv_obj_remove_flag(s_picker_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_picker_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker_scrim);
    lv_obj_move_foreground(s_picker_panel);
}

static void choice_open_cb(lv_event_t *event)
{
    show_picker((choice_control_t *)lv_event_get_user_data(event));
}

static void create_picker(lv_obj_t *parent)
{
    // The scrim is deliberately an overlay child, not a screen-level popup.
    // It catches touches outside the panel and provides a cancel action.
    s_picker_scrim = lv_obj_create(parent);
    lv_obj_set_size(s_picker_scrim, LVGL_W, LVGL_H - STATUS_BAR_H);
    lv_obj_set_pos(s_picker_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_picker_scrim, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(s_picker_scrim, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_picker_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_picker_scrim, 0, 0);
    lv_obj_add_flag(s_picker_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_picker_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_picker_scrim, picker_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_picker_scrim, LV_OBJ_FLAG_HIDDEN);

    s_picker_panel = lv_obj_create(parent);
    lv_obj_set_size(s_picker_panel, PICKER_PANEL_W, PICKER_PANEL_H);
    lv_obj_set_pos(s_picker_panel, PICKER_PANEL_X, PICKER_PANEL_Y);
    lv_obj_set_style_bg_color(s_picker_panel, lv_color_make(28, 28, 48), 0);
    lv_obj_set_style_bg_opa(s_picker_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_picker_panel, lv_color_make(105, 125, 205), 0);
    lv_obj_set_style_border_width(s_picker_panel, 3, 0);
    lv_obj_set_style_radius(s_picker_panel, 12, 0);
    lv_obj_set_style_pad_all(s_picker_panel, 0, 0);
    lv_obj_add_flag(s_picker_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_picker_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_picker_panel, LV_OBJ_FLAG_HIDDEN);

    s_picker_title = lv_label_create(s_picker_panel);
    lv_obj_set_style_text_font(s_picker_title, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(s_picker_title, lv_color_white(), 0);
    lv_obj_set_pos(s_picker_title, 40, 26);

    s_picker_hint = lv_label_create(s_picker_panel);
    lv_obj_set_style_text_font(s_picker_hint, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(s_picker_hint, lv_color_make(185, 195, 225), 0);
    lv_obj_set_pos(s_picker_hint, 40, 62);

    for (size_t i = 0; i < MAX_PICKER_OPTIONS; ++i) {
        picker_option_t *option = &s_picker_options[i];
        option->button = lv_button_create(s_picker_panel);
        lv_obj_add_flag(option->button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(option->button, 8);
        lv_obj_set_style_bg_color(option->button, lv_color_make(45, 45, 72), 0);
        lv_obj_set_style_bg_color(option->button, lv_color_make(70, 70, 112), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(option->button, lv_color_make(110, 110, 175), 0);
        lv_obj_set_style_border_width(option->button, 2, 0);
        lv_obj_set_style_radius(option->button, 8, 0);
        lv_obj_add_event_cb(option->button, picker_option_cb, LV_EVENT_CLICKED, option);

        option->label = lv_label_create(option->button);
        lv_obj_set_style_text_font(option->label, &lv_font_unscii_16, 0);
        lv_obj_set_style_text_color(option->label, lv_color_white(), 0);
        lv_obj_add_flag(option->button, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *cancel_button = lv_button_create(s_picker_panel);
    lv_obj_set_size(cancel_button, 300, 62);
    lv_obj_set_pos(cancel_button, (PICKER_PANEL_W - 300) / 2, 434);
    lv_obj_add_flag(cancel_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(cancel_button, 8);
    lv_obj_set_style_bg_color(cancel_button, lv_color_make(100, 60, 60), 0);
    lv_obj_set_style_bg_color(cancel_button, lv_color_make(145, 78, 78), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(cancel_button, lv_color_make(190, 115, 115), 0);
    lv_obj_set_style_border_width(cancel_button, 2, 0);
    lv_obj_set_style_radius(cancel_button, 8, 0);
    lv_obj_add_event_cb(cancel_button, picker_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(cancel_button);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_center(cancel_label);
}

static void create_choice_row(lv_obj_t *parent, int y_pos, const char *label_text,
                              choice_id_t id, uint8_t initial_selection)
{
    choice_control_t *control = &s_choices[choice_index(id)];
    const size_t count = choice_count(id);
    control->id = id;
    control->selected = initial_selection < count ? initial_selection : 0;
    apply_choice_to_snapshot(*control);

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_pos(label, 40, y_pos + 18);

    control->button = lv_button_create(parent);
    lv_obj_set_size(control->button, 500, 56);
    lv_obj_set_pos(control->button, 300, y_pos);
    lv_obj_add_flag(control->button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(control->button, 10);
    lv_obj_set_style_bg_color(control->button, lv_color_make(40, 40, 60), 0);
    lv_obj_set_style_bg_color(control->button, lv_color_make(64, 64, 100), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(control->button, lv_color_make(100, 100, 180), 0);
    lv_obj_set_style_border_width(control->button, 2, 0);
    lv_obj_set_style_radius(control->button, 8, 0);
    lv_obj_add_event_cb(control->button, choice_open_cb, LV_EVENT_CLICKED, control);

    control->value_label = lv_label_create(control->button);
    lv_obj_set_style_text_font(control->value_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(control->value_label, lv_color_white(), 0);
    update_choice_label(control);

    lv_obj_t *indicator = lv_label_create(control->button);
    lv_label_set_text(indicator, "v");
    lv_obj_set_style_text_font(indicator, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(indicator, lv_color_make(190, 200, 255), 0);
    lv_obj_set_pos(indicator, 468, 18);
}

// ==============================================================
// Save & Close
// ==============================================================

static void save_close_cb(lv_event_t *event)
{
    (void)event;

    // The picker consumes background touches while visible, so this callback
    // can only run after a selection panel was closed or never opened.
    const app_settings_t saved = s_current;
    if (!settings_save(&saved)) {
        ESP_LOGE(TAG, "Settings save failed");
        return;
    }

    ESP_LOGI(TAG, "Settings saved: baud=%" PRIu32 " iface=%d log=%d font=%d local_echo=%d input_mode=%d punct=%d learn_save=%d",
             saved.baud_rate, (int)saved.serial_if, (int)saved.log_level,
             (int)saved.font_size, (int)saved.local_echo, (int)saved.input_mode,
             (int)saved.punctuation_style, (int)saved.learning_save_mode);

    // The overlay must be removed before a font-size apply can rebuild all
    // terminal LVGL objects.
    settings_ui_close();
    settings_apply(&saved);

    s_current = saved;
    if (s_saved_cb != NULL) s_saved_cb(&s_current);
}

void settings_ui_set_saved_cb(settings_saved_cb_t cb)
{
    s_saved_cb = cb;
}

// ==============================================================
// Public API
// ==============================================================

void settings_ui_open(const app_settings_t *current)
{
    if (current == NULL) return;
    if (s_overlay != NULL) {
        lvgl_port_lock(0);
        lv_obj_move_foreground(s_overlay);
        lvgl_port_unlock();
        return;
    }

    s_current = *current;
    lvgl_port_lock(0);

    lv_obj_t *screen = lv_scr_act();
    s_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_overlay, LVGL_W, LVGL_H - STATUS_BAR_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_make(20, 20, 35), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_bar = lv_obj_create(s_overlay);
    lv_obj_set_size(title_bar, LVGL_W, 48);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_make(0, 80, 160), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(title_bar);
    lv_label_set_text(title, "  TAB5 Serial Terminal  -  Settings  (Ctrl+Alt+S to close)");
    lv_obj_set_style_text_font(title, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *separator = lv_obj_create(s_overlay);
    lv_obj_set_size(separator, LVGL_W, 2);
    lv_obj_set_pos(separator, 0, 48);
    lv_obj_set_style_bg_color(separator, lv_color_make(80, 80, 120), 0);
    lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(separator, 0, 0);

    create_choice_row(s_overlay, 64,  "Baud Rate:",   choice_id_t::BAUD,
                      baud_to_index(current->baud_rate));
    create_choice_row(s_overlay, 124, "Interface:",   choice_id_t::INTERFACE,
                      validated_index((int)current->serial_if, choice_count(choice_id_t::INTERFACE), 0));
    create_choice_row(s_overlay, 184, "Log Level:",   choice_id_t::LOG_LEVEL,
                      validated_index((int)current->log_level, choice_count(choice_id_t::LOG_LEVEL), 3));
    create_choice_row(s_overlay, 244, "Font Size:",   choice_id_t::FONT_SIZE,
                      validated_index((int)current->font_size, choice_count(choice_id_t::FONT_SIZE), 1));
    create_choice_row(s_overlay, 304, "Echo Back:",   choice_id_t::ECHO_BACK,
                      validated_index((int)current->local_echo, choice_count(choice_id_t::ECHO_BACK), 0));
    create_choice_row(s_overlay, 364, "Input Mode:",  choice_id_t::INPUT_MODE,
                      validated_index((int)current->input_mode, choice_count(choice_id_t::INPUT_MODE), 0));
    create_choice_row(s_overlay, 424, "Punctuation:", choice_id_t::PUNCTUATION,
                      validated_index((int)current->punctuation_style, choice_count(choice_id_t::PUNCTUATION), 0));

    choice_control_t *learning = &s_choices[choice_index(choice_id_t::LEARNING)];
    learning->id = choice_id_t::LEARNING;
    learning->selected = validated_index((int)current->learning_save_mode,
                                         choice_count(choice_id_t::LEARNING), 1);
    apply_choice_to_snapshot(*learning);

    lv_obj_t *learning_name = lv_label_create(s_overlay);
    lv_label_set_text(learning_name, "Learning:");
    lv_obj_set_style_text_font(learning_name, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(learning_name, lv_color_white(), 0);
    lv_obj_set_pos(learning_name, 820, 442);

    learning->button = lv_button_create(s_overlay);
    lv_obj_set_size(learning->button, 300, 56);
    lv_obj_set_pos(learning->button, 940, 424);
    lv_obj_add_flag(learning->button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(learning->button, 10);
    lv_obj_set_style_bg_color(learning->button, lv_color_make(75, 65, 145), 0);
    lv_obj_set_style_bg_color(learning->button, lv_color_make(105, 90, 190), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(learning->button, lv_color_make(150, 135, 230), 0);
    lv_obj_set_style_border_width(learning->button, 2, 0);
    lv_obj_set_style_radius(learning->button, 8, 0);
    lv_obj_add_event_cb(learning->button, choice_open_cb, LV_EVENT_CLICKED, learning);

    learning->value_label = lv_label_create(learning->button);
    lv_obj_set_style_text_font(learning->value_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(learning->value_label, lv_color_white(), 0);
    update_choice_label(learning);

    lv_obj_t *learning_indicator = lv_label_create(learning->button);
    lv_label_set_text(learning_indicator, "v");
    lv_obj_set_style_text_font(learning_indicator, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(learning_indicator, lv_color_make(220, 215, 255), 0);
    lv_obj_set_pos(learning_indicator, 268, 18);

    lv_obj_t *note = lv_label_create(s_overlay);
    lv_label_set_text(note,
        "  Tap a value to open its choices. Settings apply only at Save & Close.\n"
        "  Punctuation affects . , - only in Japanese input. Echo Back is local display only.\n"
        "  PortA: GPIO53/54; MBUS UART2: GPIO6/7.");
    lv_obj_set_style_text_font(note, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(note, lv_color_make(180, 180, 180), 0);
    lv_obj_set_pos(note, 40, 490);
    lv_obj_set_width(note, LVGL_W - 80);

    lv_obj_t *bottom_separator = lv_obj_create(s_overlay);
    lv_obj_set_size(bottom_separator, LVGL_W, 2);
    lv_obj_set_pos(bottom_separator, 0, 570);
    lv_obj_set_style_bg_color(bottom_separator, lv_color_make(80, 80, 120), 0);
    lv_obj_set_style_bg_opa(bottom_separator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bottom_separator, 0, 0);

    lv_obj_t *save_button = lv_button_create(s_overlay);
    lv_obj_set_size(save_button, 480, 72);
    lv_obj_set_pos(save_button, (LVGL_W - 480) / 2, (LVGL_H - STATUS_BAR_H) - 72 - 30);
    lv_obj_add_flag(save_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(save_button, 16);
    lv_obj_set_style_bg_color(save_button, lv_color_make(0, 140, 60), 0);
    lv_obj_set_style_bg_color(save_button, lv_color_make(0, 180, 80), LV_STATE_PRESSED);
    lv_obj_set_style_radius(save_button, 12, 0);
    lv_obj_add_event_cb(save_button, save_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_label = lv_label_create(save_button);
    lv_label_set_text(save_label, "Save & Close");
    lv_obj_set_style_text_font(save_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_center(save_label);

    // Create the picker after all settings controls, keeping it permanently
    // above the base settings screen while it is visible.
    create_picker(s_overlay);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Settings UI opened (fixed selection panel controls)");
}

void settings_ui_close(void)
{
    if (s_overlay == NULL) return;

    lvgl_port_lock(0);
    lv_obj_delete(s_overlay);
    s_overlay = NULL;
    s_picker_scrim = NULL;
    s_picker_panel = NULL;
    s_picker_title = NULL;
    s_picker_hint = NULL;
    s_picker_active = NULL;
    for (size_t i = 0; i < (size_t)choice_id_t::COUNT; ++i) {
        s_choices[i].button = NULL;
        s_choices[i].value_label = NULL;
    }
    for (size_t i = 0; i < MAX_PICKER_OPTIONS; ++i) {
        s_picker_options[i].control = NULL;
        s_picker_options[i].button = NULL;
        s_picker_options[i].label = NULL;
    }
    lvgl_port_unlock();

    term_mark_all_dirty();
    term_refresh_display();
    update_status_bar();
    ESP_LOGI(TAG, "Settings UI closed");
}

bool settings_ui_is_open(void)
{
    return s_overlay != NULL;
}

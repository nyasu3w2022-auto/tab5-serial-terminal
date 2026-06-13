/*
 * M5Stack TAB5 Serial Terminal - Step 1: Display & Keyboard Test
 *
 * This program initializes the TAB5 hardware, sets up the display via LVGL,
 * and reads keyboard input via I2C. It displays typed characters on screen
 * as a basic terminal emulator foundation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "m5_tab5_component.h"
#include "m5_tab5_keyboard.h"
#include "m5tab5_pinmap.h"
#include "lvgl_port.h"
#include "lvgl_port_disp.h"
#include "lvgl_port_touch.h"
#include "lvgl.h"

static const char *TAG = "terminal";

// ==============================================================
// Terminal Screen Configuration
// ==============================================================
#define TERM_COLS       106  // 1280 / 12px font width ≈ 106
#define TERM_ROWS       45   // 720 / 16px font height = 45
#define TERM_FONT_SIZE  16

// Terminal state
static char term_buffer[TERM_ROWS][TERM_COLS + 1];  // +1 for null terminator
static int cursor_row = 0;
static int cursor_col = 0;

// LVGL objects
static lv_obj_t *term_label = NULL;
static lv_obj_t *status_label = NULL;

// Keyboard instance
static m5::M5Tab5Keyboard s_keyboard;

// Board instance
static m5::tab5::m5tab5_component s_tab5_board;

// ==============================================================
// Terminal Buffer Management
// ==============================================================

static void term_clear(void)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        memset(term_buffer[r], ' ', TERM_COLS);
        term_buffer[r][TERM_COLS] = '\0';
    }
    cursor_row = 0;
    cursor_col = 0;
}

static void term_scroll_up(void)
{
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        memcpy(term_buffer[r], term_buffer[r + 1], TERM_COLS);
    }
    memset(term_buffer[TERM_ROWS - 1], ' ', TERM_COLS);
    term_buffer[TERM_ROWS - 1][TERM_COLS] = '\0';
}

static void term_newline(void)
{
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= TERM_ROWS) {
        term_scroll_up();
        cursor_row = TERM_ROWS - 1;
    }
}

static void term_put_char(char c)
{
    if (c == '\n') {
        term_newline();
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b' || c == 0x7F) {
        // Backspace
        if (cursor_col > 0) {
            cursor_col--;
            term_buffer[cursor_row][cursor_col] = ' ';
        }
    } else if (c == '\t') {
        // Tab: advance to next 8-column boundary
        int next_tab = (cursor_col + 8) & ~7;
        if (next_tab >= TERM_COLS) {
            term_newline();
        } else {
            cursor_col = next_tab;
        }
    } else if (c >= 0x20 && c < 0x7F) {
        // Printable ASCII
        term_buffer[cursor_row][cursor_col] = c;
        cursor_col++;
        if (cursor_col >= TERM_COLS) {
            term_newline();
        }
    }
}

static void term_put_string(const char *str)
{
    while (*str) {
        term_put_char(*str++);
    }
}

// ==============================================================
// LVGL Display Update
// ==============================================================

static void term_refresh_display(void)
{
    if (term_label == NULL) return;

    // Build the full screen text with newlines
    static char display_buf[TERM_ROWS * (TERM_COLS + 1) + 1];
    char *p = display_buf;

    for (int r = 0; r < TERM_ROWS; r++) {
        memcpy(p, term_buffer[r], TERM_COLS);
        p += TERM_COLS;
        *p++ = '\n';
    }
    *p = '\0';

    lvgl_port_lock(0);
    lv_label_set_text(term_label, display_buf);
    lvgl_port_unlock();
}

static void term_update_status(const char *msg)
{
    if (status_label == NULL) return;
    lvgl_port_lock(0);
    lv_label_set_text(status_label, msg);
    lvgl_port_unlock();
}

// ==============================================================
// LVGL UI Setup
// ==============================================================

static void ui_create(void)
{
    lvgl_port_lock(0);

    // Get the active screen
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Create terminal text label (monospace font)
    term_label = lv_label_create(scr);
    lv_obj_set_style_text_font(term_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(term_label, lv_color_make(0, 255, 0), 0);  // Green text
    lv_obj_set_style_text_letter_space(term_label, 0, 0);
    lv_obj_set_style_text_line_space(term_label, 0, 0);
    lv_obj_set_pos(term_label, 0, 0);
    lv_obj_set_size(term_label, 1280, 700);
    lv_label_set_long_mode(term_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(term_label, "");

    // Create status bar at bottom
    status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(status_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(status_label, lv_color_make(255, 255, 0), 0);  // Yellow
    lv_obj_set_style_bg_color(status_label, lv_color_make(0, 0, 64), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_COVER, 0);
    lv_obj_set_pos(status_label, 0, 704);
    lv_obj_set_size(status_label, 1280, 16);
    lv_label_set_text(status_label, " TAB5 Serial Terminal - Step 1: Keyboard Test");

    lvgl_port_unlock();
}

// ==============================================================
// Keyboard Event Handling
// ==============================================================

// Queue for keyboard events
static QueueHandle_t s_key_queue = NULL;

typedef struct {
    uint8_t modifier;
    char str[16];
} key_event_msg_t;

static void keyboard_event_cb(m5_tab5_key_event_t event, void *arg)
{
    if (event.type == M5_TAB5_KB_MODE_STRING && event.str_len > 0) {
        key_event_msg_t msg;
        msg.modifier = event.str_modifier;
        memset(msg.str, 0, sizeof(msg.str));
        memcpy(msg.str, event.str_data, event.str_len < 15 ? event.str_len : 15);

        if (s_key_queue != NULL) {
            xQueueSend(s_key_queue, &msg, 0);
        }
    }
}

// ==============================================================
// LCD/LVGL Initialization (based on official M5Tab5-Keyboard-UserDemo)
// ==============================================================

// Display resolution constants
static constexpr uint32_t LCD_H_RES = 720;
static constexpr uint32_t LCD_V_RES = 1280;

static lv_display_t *s_lvgl_disp = nullptr;
static lv_indev_t   *s_lvgl_touch_indev = nullptr;

static esp_err_t app_lcd_lvgl_init(m5::tab5::m5tab5_component &board)
{
    if (s_lvgl_disp != nullptr) {
        ESP_LOGW(TAG, "LVGL already initialized, skipping");
        return ESP_OK;
    }

    esp_lcd_panel_handle_t panel_handle = board.lcd_panel();
    if (panel_handle == nullptr) {
        ESP_LOGE(TAG, "LCD panel handle unavailable, call board.begin() first");
        return ESP_ERR_INVALID_STATE;
    }

    // Initialize LVGL port
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

    // Add MIPI-DSI display
    lvgl_disp_cfg_t disp_cfg    = {};
    disp_cfg.panel_handle       = panel_handle;
    disp_cfg.hres               = LCD_H_RES;
    disp_cfg.vres               = LCD_V_RES;
    disp_cfg.buffer_size        = LCD_H_RES * LCD_V_RES;
    disp_cfg.color_format       = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.full_refresh = 0;
    disp_cfg.flags.direct_mode  = 1;
    disp_cfg.flags.buff_spiram  = 1;
    disp_cfg.flags.sw_rotate    = 1;  // 90-degree rotation

    lvgl_disp_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.sw_rotation         = LV_DISPLAY_ROTATION_90;
    dsi_cfg.flags.avoid_tearing = 1;
    dsi_cfg.flags.use_ppa       = 1;

    s_lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (s_lvgl_disp == nullptr) {
        ESP_LOGE(TAG, "Failed to add LVGL DSI display");
        return ESP_FAIL;
    }

    // Add touch input (optional)
    esp_lcd_touch_handle_t touch_handle = board.touch_panel();
    if (touch_handle != nullptr) {
        lvgl_touch_cfg_t touch_cfg = {};
        touch_cfg.disp             = s_lvgl_disp;
        touch_cfg.handle           = touch_handle;
        s_lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (s_lvgl_touch_indev == nullptr) {
            ESP_LOGW(TAG, "Failed to add LVGL touch input (non-fatal)");
        } else {
            lvgl_port_set_touch_rotation(s_lvgl_touch_indev, LV_DISPLAY_ROTATION_90);
        }
    }

    return ESP_OK;
}

// ==============================================================
// Main Application
// ==============================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== TAB5 Serial Terminal - Step 1 ===");
    ESP_LOGI(TAG, "Initializing TAB5 hardware...");

    // Initialize board
    m5::tab5::m5tab5_component_config_t board_cfg = {};
    esp_err_t ret = s_tab5_board.begin(board_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(ret));
        return;
    }

    const m5::tab5::m5tab5_variant_descriptor_t *variant = s_tab5_board.variant();
    if (variant != nullptr) {
        ESP_LOGI(TAG, "Detected variant: %s", variant->id);
    }

    // Enable USB-A 5V power (for future USB serial host use)
    s_tab5_board.usb5v_enable(true);
    ESP_LOGI(TAG, "USB-A 5V power enabled");

    // Initialize LCD and LVGL
    ret = app_lcd_lvgl_init(s_tab5_board);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD/LVGL init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "LCD/LVGL initialized");

    // Create UI
    ui_create();
    ESP_LOGI(TAG, "UI created");

    // Initialize terminal buffer
    term_clear();
    term_put_string("M5Stack TAB5 Serial Terminal\n");
    term_put_string("============================\n");
    term_put_string("Step 1: Hardware Verification\n");
    term_put_string("\n");
    term_put_string("Keyboard: ");
    term_refresh_display();

    // Create keyboard event queue
    s_key_queue = xQueueCreate(32, sizeof(key_event_msg_t));

    // Initialize keyboard
    ESP_LOGI(TAG, "Initializing keyboard...");
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
        ESP_LOGI(TAG, "Keyboard OK, FW version: 0x%02X", version);

        char msg[64];
        snprintf(msg, sizeof(msg), "OK (FW: 0x%02X)\n", version);
        term_put_string(msg);

        // Set to String mode for character input
        s_keyboard.enableStringMode(keyboard_event_cb, NULL);
        ESP_LOGI(TAG, "Keyboard set to String mode");

        term_put_string("Mode: Character (String)\n");
        term_put_string("\n");
        term_put_string("Type something:\n");
        term_put_string("> ");
    } else {
        ESP_LOGW(TAG, "Keyboard not detected (err=%d)", kb_err);
        term_put_string("NOT DETECTED\n");
        term_put_string("\nPlease connect the keyboard accessory.\n");
    }

    term_refresh_display();

    // Status bar info
    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg),
             " TAB5 Terminal | Keyboard: %s | Ctrl+C=Clear | Ready",
             (kb_err == M5_TAB5_KB_OK) ? "Connected" : "Disconnected");
    term_update_status(status_msg);

    // Main loop: process keyboard events
    ESP_LOGI(TAG, "Entering main loop...");
    key_event_msg_t key_msg;

    while (1) {
        if (xQueueReceive(s_key_queue, &key_msg, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool ctrl_pressed = (key_msg.modifier & 0x01) != 0;
            bool alt_pressed = (key_msg.modifier & 0x04) != 0;

            ESP_LOGD(TAG, "Key: '%s' mod=0x%02X ctrl=%d alt=%d",
                     key_msg.str, key_msg.modifier, ctrl_pressed, alt_pressed);

            if (ctrl_pressed && (key_msg.str[0] == 'c' || key_msg.str[0] == 'C')) {
                // Ctrl+C: Clear screen
                term_clear();
                term_put_string("Screen cleared.\n> ");
                term_refresh_display();
                continue;
            }

            if (ctrl_pressed && (key_msg.str[0] == 'l' || key_msg.str[0] == 'L')) {
                // Ctrl+L: Redraw
                term_refresh_display();
                continue;
            }

            // Process each character in the string
            for (int i = 0; key_msg.str[i] != '\0'; i++) {
                char c = key_msg.str[i];

                if (c == '\r' || c == '\n') {
                    term_put_char('\n');
                    term_put_string("> ");
                } else if (c == '\b' || c == 0x7F) {
                    term_put_char('\b');
                } else {
                    term_put_char(c);
                }
            }

            term_refresh_display();
        }
    }
}

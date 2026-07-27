#pragma once
/*
 * display.h — LVGL-based terminal display rendering.
 *
 * This module owns:
 *   - LCD/LVGL initialization (app_lcd_lvgl_init)
 *   - LVGL UI creation: terminal canvas rows, status bar (ui_create)
 *   - Per-row dirty-flag rendering (term_refresh_display)
 *   - Status bar text update (update_status_bar)
 *   - Cursor blink timer
 *
 * It reads term_buffer, cursor state, and row_dirty[] from terminal.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <esp_err.h>
#include "m5_tab5_component.h"

/**
 * @brief Initialize LCD and LVGL port.
 * @param board  Reference to the initialized TAB5 board component.
 * @return ESP_OK on success.
 */
esp_err_t app_lcd_lvgl_init(m5::tab5::m5tab5_component &board);

/**
 * @brief Create LVGL UI objects (terminal canvas rows, status bar, cursor timer).
 * Must be called after app_lcd_lvgl_init().
 */
void ui_create(void);

/**
 * @brief Redraw all dirty rows and update cursor display.
 * Must be called from the main task (not from LVGL task).
 */
void term_refresh_display(void);

/**
 * @brief Update the status bar text with current USB/baud state.
 */
void update_status_bar(void);

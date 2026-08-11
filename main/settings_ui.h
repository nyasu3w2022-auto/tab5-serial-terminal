#pragma once
/*
 * settings_ui.h — LVGL-based settings screen.
 *
 * The settings screen is a full-screen overlay that appears on top of
 * the terminal canvas when the user presses Ctrl+Alt+S.  When the user
 * presses "Save & Close" or Ctrl+Alt+S again, the overlay is destroyed
 * and the terminal canvas is restored.
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings.h"
#include <stdbool.h>

/**
 * @brief Callback invoked after settings are saved and applied.
 *
 * The caller (main.cpp) registers this callback to keep its local
 * s_settings copy in sync with whatever was saved in the settings screen.
 *
 * @param saved  Pointer to the newly saved settings.
 */
typedef void (*settings_saved_cb_t)(const app_settings_t *saved);

/**
 * @brief Register a callback to be called after Save & Close.
 *
 * Must be called once at startup before the settings screen is opened.
 *
 * @param cb  Callback function, or NULL to clear.
 */
void settings_ui_set_saved_cb(settings_saved_cb_t cb);

/**
 * @brief Open the settings screen.
 *
 * Creates the LVGL overlay and populates it with the current settings.
 * Must be called from the main task (LVGL lock is acquired internally).
 *
 * @param current  Current settings to display as initial values.
 */
void settings_ui_open(const app_settings_t *current);

/**
 * @brief Close the settings screen.
 *
 * Destroys the LVGL overlay.  Called automatically by the "Save & Close"
 * button; can also be called programmatically (e.g. second Ctrl+Alt+S).
 */
void settings_ui_close(void);

/**
 * @brief Returns true if the settings screen is currently visible.
 */
bool settings_ui_is_open(void);

#pragma once
/*
 * ime_ui.h — LVGL overlay for TAB5 local Japanese input.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ime_skk.h"

/** Refresh the IME overlay.  Passing false hides the overlay. */
void ime_ui_update(bool japanese_mode, const ime_skk_t &ime);

/** Remove the IME overlay when the terminal UI is rebuilt or input mode changes. */
void ime_ui_hide(void);

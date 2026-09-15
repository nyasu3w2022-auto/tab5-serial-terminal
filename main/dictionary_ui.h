#pragma once
/*
 * dictionary_ui.h — Local user-dictionary editor overlay for TAB5 SKK.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include <string>

#include "ime_skk.h"

/** Open the full-screen local dictionary editor for the supplied IME. */
void dictionary_ui_open(ime_skk_t *ime);

/** Close the dictionary editor and discard uncommitted field text. */
void dictionary_ui_close(void);

/** Return true while the dictionary editor owns the foreground UI. */
bool dictionary_ui_is_open(void);

/** Add or promote the current Reading + Okuri + Candidate entry. */
bool dictionary_ui_add_or_promote(void);

/** Delete the exact current Reading + Okuri + Candidate entry. */
bool dictionary_ui_delete_exact(void);

/**
 * Return true when printable input should be written directly to the focused
 * field instead of being handled by the Japanese IME. This is only true for
 * the one-character ASCII Okuri field.
 */
bool dictionary_ui_accept_raw_text(const char *text, size_t len);

/** Return true when the focused field receives text committed by the IME. */
bool dictionary_ui_accept_ime_commit(const std::string &text);

/**
 * Handle a named physical key. Returns true when the editor consumed it.
 * Backspace/Escape return false while an IME preedit is active so that the
 * caller can apply normal IME editing/cancel behavior first.
 */
bool dictionary_ui_handle_special_key(const char *name, const ime_skk_t &ime);

/** Refresh field values, focus styling, status, and optional IME preedit. */
void dictionary_ui_update(const ime_skk_t &ime);

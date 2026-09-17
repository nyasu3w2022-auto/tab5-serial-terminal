/*
 * main.cpp — TAB5 Serial Terminal: application entry point and main loop.
 *
 * Architecture:
 *   terminal.cpp    — VT100 parser and terminal buffer management
 *   display.cpp     — LVGL-based display rendering and UI
 *   usb_serial.cpp  — USB Host CDC-ACM/VCP, keyboard events, screen log
 *   settings.cpp    — NVS-backed persistent settings
 *   settings_ui.cpp — LVGL settings screen overlay
 *   main.cpp        — app_main, main loop, keyboard dispatch
 *
 * Keyboard shortcuts (local, not sent to remote):
 *   Ctrl+C        — Clear terminal screen
 *   Ctrl+L        — Force full redisplay
 *   Ctrl+Alt+S    — Open / close settings screen
 *   Ctrl+Alt+D    — Open / close local SKK Dictionary Editor
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <esp_spiffs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

#include "m5_tab5_component.h"
#include "m5_tab5_keyboard.h"
#include "m5tab5_pinmap.h"

#include "terminal.h"
#include "display.h"
#include "usb_serial.h"          // shared RX ring buffer, logs, keyboard queue
#include "serial_transport.h"    // active USB / Port A / MBUS UART transport
#include "settings.h"
#include "settings_ui.h"
#include "ime_skk.h"
#include "ime_ui.h"
#include "dictionary_ui.h"

static const char *TAG = "main";

static m5::tab5::m5tab5_component s_tab5_board;
static m5::M5Tab5Keyboard         s_keyboard;

// Current application settings (loaded from NVS at boot)
static app_settings_t s_settings = {};

// Local Japanese input state.  Conversion occurs on TAB5 and only committed
// UTF-8 text is forwarded through serial_transport_tx().
static ime_skk_t s_ime;
static bool s_japanese_input_active = false;
static constexpr const char *SKK_DICT_PATH = "/skk/SKK-JISYO.S.txt";
static constexpr const char *SKK_SUPPLEMENT_DICT_PATH = "/skk/SKK-JISYO.TAB5.txt";
static constexpr const char *SKK_USER_DICT_PATH = "/skk-user/SKK-JISYO.user.txt";
static constexpr size_t LEARNING_FLUSH_THRESHOLD = 16;
static constexpr TickType_t LEARNING_FLUSH_IDLE_TICKS = pdMS_TO_TICKS(60000);
static uint32_t s_last_learning_generation = 0;
static TickType_t s_pending_learning_since = 0;
static TickType_t s_deferred_retry_after = 0;

static void refresh_ime_input_indicator(void)
{
    display_set_japanese_input_mode(
        s_japanese_input_active,
        s_japanese_input_active && s_ime.kana_mode() == ime_kana_mode_t::KATAKANA);
    update_status_bar();
}

static void apply_ime_punctuation_style(app_punctuation_style_t style)
{
    switch (style) {
    case PUNCTUATION_ASCII:
        s_ime.set_punctuation_style(ime_punctuation_style_t::ASCII);
        break;
    case PUNCTUATION_FULLWIDTH:
        s_ime.set_punctuation_style(ime_punctuation_style_t::FULLWIDTH);
        break;
    case PUNCTUATION_JAPANESE:
    default:
        s_ime.set_punctuation_style(ime_punctuation_style_t::JAPANESE);
        break;
    }
}

static void set_japanese_input_active(bool active)
{
    if (s_japanese_input_active != active) {
        s_ime.reset();
        ime_ui_hide();
    }
    s_japanese_input_active = active;
    refresh_ime_input_indicator();
}

static void apply_ime_learning_save_mode(app_learning_save_mode_t mode)
{
    switch (mode) {
    case LEARNING_SAVE_OFF:
        s_ime.set_learning_mode(ime_learning_mode_t::OFF);
        break;
    case LEARNING_SAVE_MANUAL:
        s_ime.set_learning_mode(ime_learning_mode_t::MANUAL);
        break;
    case LEARNING_SAVE_DEFERRED:
    default:
        s_ime.set_learning_mode(ime_learning_mode_t::DEFERRED);
        break;
    }
    s_last_learning_generation = s_ime.learning_generation();
    s_pending_learning_since = s_ime.pending_learning_count() ? xTaskGetTickCount() : 0;
    s_deferred_retry_after = 0;
}

static void service_deferred_learning(void)
{
    if (s_ime.learning_mode() != ime_learning_mode_t::DEFERRED) return;

    const size_t pending = s_ime.pending_learning_count();
    if (pending == 0) {
        s_pending_learning_since = 0;
        s_deferred_retry_after = 0;
        s_last_learning_generation = s_ime.learning_generation();
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const uint32_t generation = s_ime.learning_generation();
    if (generation != s_last_learning_generation) {
        s_last_learning_generation = generation;
        s_pending_learning_since = now;
        s_deferred_retry_after = 0;
    }
    if (s_deferred_retry_after != 0 &&
        (int32_t)(now - s_deferred_retry_after) < 0) return;

    const bool threshold_reached = pending >= LEARNING_FLUSH_THRESHOLD;
    const bool idle_timeout = s_pending_learning_since != 0 &&
                              (now - s_pending_learning_since) >= LEARNING_FLUSH_IDLE_TICKS;
    if (!threshold_reached && !idle_timeout) return;

    if (s_ime.flush_pending_learning()) {
        ESP_LOGI(TAG, "Saved %u deferred SKK learning entries", (unsigned)pending);
        s_last_learning_generation = s_ime.learning_generation();
        s_pending_learning_since = 0;
        s_deferred_retry_after = 0;
    } else {
        // Avoid retrying on every loop if the writable SPIFFS partition is
        // temporarily unavailable. The next retry occurs after another idle interval.
        ESP_LOGW(TAG, "Deferred SKK learning save failed; will retry later");
        s_deferred_retry_after = now + LEARNING_FLUSH_IDLE_TICKS;
    }
}

static void init_ime_dictionary_storage(void)
{
    // The bundled dictionary is immutable firmware content.  Learned user
    // candidates live in a separate runtime-writable SPIFFS partition, so a
    // subsequent dictionary image flash does not erase them.
    esp_vfs_spiffs_conf_t system_conf = {};
    system_conf.base_path = "/skk";
    system_conf.partition_label = "skk";
    system_conf.max_files = 1;
    system_conf.format_if_mount_failed = false;

    esp_err_t err = esp_vfs_spiffs_register(&system_conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SKK system SPIFFS mount unavailable: %s", esp_err_to_name(err));
        s_ime.set_dictionary_path(NULL);
        s_ime.set_supplement_dictionary_path(NULL);
        s_ime.set_user_dictionary_path(NULL);
        return;
    }

    esp_vfs_spiffs_conf_t user_conf = {};
    user_conf.base_path = "/skk-user";
    user_conf.partition_label = "userdict";
    // Updating a user entry keeps the old file open while writing a
    // temporary replacement. Both files must be available simultaneously.
    user_conf.max_files = 2;
    // A newly flashed user partition is intentionally blank. Format it once
    // on first boot, but never format the system dictionary partition.
    user_conf.format_if_mount_failed = true;
    esp_err_t user_err = esp_vfs_spiffs_register(&user_conf);

    s_ime.set_dictionary_path(SKK_DICT_PATH);
    s_ime.set_supplement_dictionary_path(SKK_SUPPLEMENT_DICT_PATH);
    s_ime.set_user_dictionary_path(user_err == ESP_OK ? SKK_USER_DICT_PATH : NULL);
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(system_conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SKK system SPIFFS mounted: used=%u/%u bytes",
                 (unsigned)used, (unsigned)total);
    }
    if (user_err != ESP_OK) {
        ESP_LOGW(TAG, "SKK user dictionary disabled: %s", esp_err_to_name(user_err));
    }
    if (s_ime.dictionary_available()) {
        ESP_LOGI(TAG, "SKK dictionary ready: %s", SKK_DICT_PATH);
        ESP_LOGI(TAG, "SKK supplementary dictionary: %s", SKK_SUPPLEMENT_DICT_PATH);
        ESP_LOGI(TAG, "SKK user dictionary: %s",
                 s_ime.user_dictionary_available() ? "loaded" : "will be created on first learned candidate");
    } else {
        ESP_LOGW(TAG, "SKK dictionary not found: %s (hiragana input remains available)",
                 SKK_DICT_PATH);
    }
}

// Called by settings_ui after Save & Close so future openings use the
// newly saved values rather than the boot-time snapshot.
static void on_settings_saved(const app_settings_t *saved)
{
    if (saved) {
        s_settings = *saved;
        apply_ime_punctuation_style(s_settings.punctuation_style);
        apply_ime_learning_save_mode(s_settings.learning_save_mode);
        set_japanese_input_active(s_settings.input_mode == INPUT_MODE_JAPANESE);
        if (s_japanese_input_active && s_ime.state() != ime_state_t::IDLE) {
            ime_ui_update(true, s_ime);
        }
        ESP_LOGI(TAG, "Current settings synchronized after save");
    }
}

// vt100_tx_cb_t intentionally has a void return type. The transport layer
// returns esp_err_t to let direct keyboard sends observe failures, whereas
// VT100 control responses (DSR/DA) have no caller to receive that status.
static void vt100_transport_tx_cb(const uint8_t *data, size_t len)
{
    esp_err_t err = serial_transport_tx(data, len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "VT100 response TX via %s failed: %s",
                 serial_transport_get_name(), esp_err_to_name(err));
    }
}

// ==============================================================
// Keyboard Input Dispatch
// ==============================================================

// Special key name → VT100 sequence table (static, not rebuilt each loop)
static const struct {
    const char *name;
    const char *seq;
} s_special_keys[] = {
    { "enter",     "\r"         },
    { "backspace", "\x7f"       },  // DEL (most terminals expect 0x7F for backspace)
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
    { "del",       "\x1b[3~"    },  // TAB5 keyboard sends "del" (not "delete")
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
    { "esc",       "\x1b"       },  // TAB5 keyboard sends "esc" (not "escape")
    { NULL, NULL }
};

/**
 * @brief Render a locally echoed special key after successful transmission.
 *
 * Home, End, Up, Down, Page and function keys intentionally have no local
 * rendering: their intended effect depends on the remote application's
 * current line or screen state. Left/Right and editing keys are safe to
 * reflect in the local terminal buffer.
 */
static bool local_echo_special_key(const char *name)
{
    if (s_settings.local_echo != LOCAL_ECHO_ON) return false;

    if (strcasecmp(name, "enter") == 0) {
        term_local_echo_enter();
    } else if (strcasecmp(name, "tab") == 0) {
        term_local_echo_tab();
    } else if (strcasecmp(name, "backspace") == 0) {
        term_local_echo_backspace();
    } else if (strcasecmp(name, "delete") == 0 || strcasecmp(name, "del") == 0) {
        term_local_echo_delete();
    } else if (strcasecmp(name, "left") == 0) {
        term_local_echo_cursor_left();
    } else if (strcasecmp(name, "right") == 0) {
        term_local_echo_cursor_right();
    } else {
        return false;
    }
    return true;
}

static bool transmit_ime_commit(const std::string &text)
{
    if (text.empty()) return false;

    esp_err_t err = serial_transport_tx((const uint8_t *)text.data(), text.size());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Japanese IME commit TX failed via %s: %s",
                 serial_transport_get_name(), esp_err_to_name(err));
        return false;
    }
    if (s_settings.local_echo == LOCAL_ECHO_ON) {
        term_local_echo_text((const uint8_t *)text.data(), text.size());
    }
    return true;
}

static void ime_cancel_before_remote_input(void)
{
    if (!s_japanese_input_active || s_ime.state() == ime_state_t::IDLE) return;

    // A remote terminal editing command cannot operate on TAB5's local
    // preedit.  Discard the local preedit first so the overlay and remote
    // line cannot silently diverge.
    s_ime.reset();
    ime_ui_update(true, s_ime);
    refresh_ime_input_indicator();
}

static bool ime_commit_without_terminal_cr(void)
{
    if (!s_japanese_input_active || s_ime.state() == ime_state_t::IDLE) return false;

    ime_result_t result = s_ime.input_key(ime_key_t::COMMIT);
    if (!result.consumed) return false;
    if (!result.commit.empty()) transmit_ime_commit(result.commit);
    ime_ui_update(true, s_ime);
    refresh_ime_input_indicator();
    return true;
}

static bool dictionary_editor_commit_ime(void)
{
    if (!dictionary_ui_is_open()) return false;
    if (s_ime.state() == ime_state_t::IDLE) {
        // Ctrl+J is local to the editor even with no pending preedit.
        dictionary_ui_update(s_ime);
        return true;
    }

    const bool candidate_active = s_ime.state() == ime_state_t::CANDIDATE;
    const std::string candidate_stem = candidate_active
                                      ? s_ime.candidate_at(s_ime.candidate_index()) : "";
    ime_result_t result = s_ime.input_key(ime_key_t::COMMIT);
    if (!result.consumed) return true;
    // A dictionary candidate is stored as its SKK stem. In particular,
    // selecting くr /来/ must put "来" rather than the committed "来る"
    // into a field that will later be used with another okurigana key.
    const std::string &field_text = candidate_active ? candidate_stem : result.commit;
    if (!field_text.empty()) dictionary_ui_accept_ime_commit(field_text);
    dictionary_ui_update(s_ime);
    return true;
}

static bool dictionary_editor_handle_ime_special(const char *name)
{
    if (!dictionary_ui_is_open() || name == nullptr) return false;

    ime_key_t key;
    if (strcasecmp(name, "backspace") == 0) {
        key = ime_key_t::BACKSPACE;
    } else if (strcasecmp(name, "left") == 0) {
        key = ime_key_t::LEFT;
    } else if (strcasecmp(name, "right") == 0) {
        key = ime_key_t::RIGHT;
    } else if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0) {
        key = ime_key_t::ESCAPE;
    } else {
        return false;
    }

    ime_result_t result = s_ime.input_key(key);
    if (!result.consumed) return false;
    if (!result.commit.empty()) dictionary_ui_accept_ime_commit(result.commit);
    dictionary_ui_update(s_ime);
    return true;
}

static bool ime_handle_special_key(const char *name)
{
    if (!s_japanese_input_active) return false;

    ime_key_t key;
    if (strcasecmp(name, "enter") == 0) {
        key = ime_key_t::ENTER;
    } else if (strcasecmp(name, "backspace") == 0) {
        key = ime_key_t::BACKSPACE;
    } else if (strcasecmp(name, "left") == 0) {
        key = ime_key_t::LEFT;
    } else if (strcasecmp(name, "right") == 0) {
        key = ime_key_t::RIGHT;
    } else if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0) {
        key = ime_key_t::ESCAPE;
    } else {
        return false;
    }

    ime_result_t result = s_ime.input_key(key);
    if (!result.consumed) return false;
    if (!result.commit.empty()) transmit_ime_commit(result.commit);
    // Synchronize even when a result only commits text.  ime_ui_update()
    // hides the overlay for IDLE, preventing a stale preedit after Enter.
    ime_ui_update(true, s_ime);
    refresh_ime_input_indicator();
    return true;
}

/**
 * @brief Handle one keyboard event from the key queue.
 *
 * modifier bits (str_modifier from TAB5 keyboard):
 *   0x01 = Ctrl
 *   0x04 = Alt
 *   0x05 = Ctrl+Alt
 *
 * @return true if display refresh is needed.
 */
static bool handle_key_event(const key_event_msg_t *msg)
{
    bool ctrl = (msg->modifier & 0x01) != 0;
    bool alt  = (msg->modifier & 0x04) != 0;
    const size_t text_len = strlen(msg->str);

    // ---- Ctrl+Alt combinations (local shortcuts, never sent to remote) ----
    if (ctrl && alt) {
        char k = (char)toupper((unsigned char)msg->str[0]);
        // The Dictionary Editor owns its local IME session. Only its own
        // close shortcut and the settings shortcut are allowed through.
        if (dictionary_ui_is_open() && k != 'D' && k != 'S') return true;
        if (k == 'S') {
            // Ctrl+Alt+S: toggle settings screen. The Dictionary Editor is a
            // separate full-screen local UI, so close it before opening settings.
            if (dictionary_ui_is_open()) {
                s_ime.reset();
                dictionary_ui_close();
            }
            if (settings_ui_is_open()) {
                settings_ui_close();
                if (s_japanese_input_active && s_ime.state() != ime_state_t::IDLE) {
                    ime_ui_update(true, s_ime);
                }
            } else {
                // The settings overlay must be the foremost full-screen UI.
                ime_ui_hide();
                settings_ui_open(&s_settings);
            }
            return true;
        }
        if (k == 'J') {
            // Ctrl+Alt+J: temporary local input-mode toggle.  The setting
            // dropdown controls the mode used after the next reboot.
            set_japanese_input_active(!s_japanese_input_active);
            ESP_LOGI(TAG, "Japanese input %s", s_japanese_input_active ? "enabled" : "disabled");
            return true;
        }
        if (k == 'D') {
            // Avoid discarding unsaved settings. The settings UI owns its
            // keyboard focus until the user explicitly closes it.
            if (settings_ui_is_open()) return true;
            // Ctrl+Alt+D: Dictionary Editor is fully local. It can be used
            // even when Japanese input is temporarily disabled.
            if (dictionary_ui_is_open()) {
                s_ime.reset();
                dictionary_ui_close();
            } else {
                // The editor owns the same IME instance. A terminal preedit
                // has no dictionary-field destination, so discard it first.
                s_ime.reset();
                ime_ui_hide();
                dictionary_ui_open(&s_ime);
            }
            return true;
        }
        // Other Ctrl+Alt combinations: silently ignore (don't send to remote)
        return false;
    }

    // ---- If settings screen is open, swallow all other keys ----
    if (settings_ui_is_open()) {
        return false;
    }

    // ---- Dictionary Editor owns every key while it is open ----
    if (dictionary_ui_is_open()) {
        if (ctrl && !alt) {
            const char k = (char)toupper((unsigned char)msg->str[0]);
            if (k == 'J') return dictionary_editor_commit_ime();
            if (k == 'S') return dictionary_ui_add_or_promote();
            if (k == 'X') return dictionary_ui_delete_exact();
            if (k == 'W') return dictionary_ui_save_learning();
            if (k == 'E') return dictionary_ui_export_to_sd();
            if (k == 'I') return dictionary_ui_import_merge_from_sd();
            if (k == 'R') return dictionary_ui_import_replace_from_sd();
            // No other Ctrl operation is meaningful in the local editor.
            return true;
        }
        if (alt) return true;
        if (dictionary_ui_handle_special_key(msg->str, s_ime)) {
            return true;
        }
        if (dictionary_editor_handle_ime_special(msg->str)) {
            return true;
        }
        // Any named terminal key not understood by the editor is swallowed;
        // it must never become a literal dictionary field value or remote TX.
        for (int i = 0; s_special_keys[i].name != NULL; ++i) {
            if (strcasecmp(msg->str, s_special_keys[i].name) == 0) return true;
        }
        if (text_len > 0) {
            if (dictionary_ui_accept_raw_text(msg->str, text_len)) return true;
            ime_result_t result = s_ime.input_text(msg->str, text_len);
            if (!result.commit.empty()) dictionary_ui_accept_ime_commit(result.commit);
            // Raw text that is not consumed (temporary ASCII mode) is still
            // swallowed: dictionary editing must never transmit to the peer.
            dictionary_ui_update(s_ime);
            return true;
        }
        return true;
    }

    // ---- Ctrl-only combinations ----
    if (ctrl) {
        char k = (char)toupper((unsigned char)msg->str[0]);

        if (k == 'C') {
            // Ctrl+C: clear screen locally. Clear a local preedit first so it
            // cannot remain as an overlay over the newly cleared terminal.
            ime_cancel_before_remote_input();
            term_clear_all();
            const char *m = "\033[1;32m[Screen cleared]\033[0m\n";
            for (const char *p = m; *p; p++) vt100_process_byte((uint8_t)*p);
            return true;
        }
        if (k == 'L') {
            // Ctrl+L: force full redisplay. It must not leave an unrelated
            // local preedit active after the terminal is redrawn.
            ime_cancel_before_remote_input();
            term_mark_all_dirty();
            return true;
        }
        if (k == 'J' && ime_commit_without_terminal_cr()) {
            // Ctrl+J is the standard SKK confirmation key.  It commits the
            // local reading/candidate but intentionally does not send LF/CR.
            return true;
        }

        // Ctrl+ESC → send ESC
        if (strcasecmp(msg->str, "escape") == 0 || strcasecmp(msg->str, "esc") == 0) {
            ime_cancel_before_remote_input();
            uint8_t esc = 0x1B;
            serial_transport_tx(&esc, 1);
            return false;
        }

        // Other Ctrl+key: send as control character (e.g. Ctrl+D → 0x04).
        // Cancel any local preedit first because the remote application will
        // act on this control byte independently of the IME.
        if (k >= '@' && k <= '_') {
            ime_cancel_before_remote_input();
            uint8_t ctrl_char = (uint8_t)(k - '@');
            serial_transport_tx(&ctrl_char, 1);
        }
        return false;
    }

    // ---- Alt-only combinations: send ESC + key (standard terminal convention) ----
    if (alt) {
        ime_cancel_before_remote_input();
        uint8_t esc = 0x1B;
        serial_transport_tx(&esc, 1);
        // Fall through to send the key itself
    }

    // Special key names → VT100 sequences
    for (int i = 0; s_special_keys[i].name != NULL; i++) {
        if (strcasecmp(msg->str, s_special_keys[i].name) == 0) {
            if (!alt && ime_handle_special_key(msg->str)) return true;
            // Tab, Delete, navigation, function keys and Alt-modified keys
            // are terminal operations.  They must not leave an unrelated
            // local IME preedit visible after their VT100 sequence is sent.
            ime_cancel_before_remote_input();
            const char *seq = s_special_keys[i].seq;
            esp_err_t err = serial_transport_tx((const uint8_t *)seq, strlen(seq));
            return (err == ESP_OK) && !alt && local_echo_special_key(msg->str);
        }
    }

    // Japanese input consumes preedit keys locally and forwards only the
    // committed UTF-8 result.  Alt-modified input keeps terminal ESC-prefix
    // semantics and is never routed through the local IME.
    if (s_japanese_input_active && !alt && text_len > 0) {
        ime_result_t ime_result = s_ime.input_text(msg->str, text_len);
        if (ime_result.consumed) {
            if (!ime_result.commit.empty()) transmit_ime_commit(ime_result.commit);
            // Keep the overlay lifecycle strictly tied to the IME state for
            // every consumed input, including candidate/direct confirmation.
            ime_ui_update(true, s_ime);
            refresh_ime_input_indicator();
            return ime_result.changed || !ime_result.commit.empty();
        }
    }

    // Normal printable characters are always sent to the selected transport.
    // With Local Echo enabled, they are additionally rendered here only after
    // successful TX; no configuration sequence is sent to the serial peer.
    if (text_len == 0) return false;
    esp_err_t err = serial_transport_tx((const uint8_t *)msg->str, text_len);
    if (err == ESP_OK && !alt && s_settings.local_echo == LOCAL_ECHO_ON) {
        term_local_echo_text((const uint8_t *)msg->str, text_len);
        return true;
    }
    return false;
}

// ==============================================================
// Application Entry Point
// ==============================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== TAB5 Serial Terminal ===");

    // ---- NVS init (required for settings persistence) ----
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased and re-initialized");
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_ret));
    }

    // ---- Load settings from NVS ----
    settings_load(&s_settings);
    apply_ime_punctuation_style(s_settings.punctuation_style);
    settings_ui_set_saved_cb(on_settings_saved);

    // ---- Apply font size from settings BEFORE ui_create() ----
    // term_set_font_size() sets g_term_cols/rows used by ui_create().
    // (baud rate and log level are applied after USB init below)
    if (s_settings.font_size == FONT_SIZE_SMALL) {
        term_set_font_size(8, 16);
    } else {
        term_set_font_size(14, 28);
    }

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

    // Initial welcome message (through VT100 parser)
    const char *welcome =
        "\033[2J\033[H"
        "\033[1;32mM5Stack TAB5 Serial Terminal\033[0m\r\n"
        "\033[32m============================\033[0m\r\n"
        "Initializing serial interface...\r\n";
    for (const char *p = welcome; *p; p++) vt100_process_byte((uint8_t)*p);
    term_refresh_display();

    // ---- Shared serial infrastructure (queues, RX ring buffer, logs) ----
    usb_init();
    init_ime_dictionary_storage();
    apply_ime_learning_save_mode(s_settings.learning_save_mode);
    serial_transport_init();
    // DSR/DA terminal responses use whichever transport is currently active.
    vt100_set_tx_cb(vt100_transport_tx_cb);

    // ---- Apply serial interface, baud rate and log-level settings ----
    settings_apply(&s_settings);
    set_japanese_input_active(s_settings.input_mode == INPUT_MODE_JAPANESE);

    // USB needs host/CDC driver initialization before it can enumerate a device.
    // Port A UART is ready immediately after its driver is configured.
    if (!serial_transport_wait_ready(10000)) {
        ESP_LOGW(TAG, "%s transport did not become ready within timeout",
                 serial_transport_get_name());
        const char *warn = "[Serial] Transport initialization timed out.\r\n";
        for (const char *p = warn; *p; p++) vt100_process_byte((uint8_t)*p);
    }
    ESP_LOGI(TAG, "%s transport initialized, starting main loop",
             serial_transport_get_name());

    const char *ready_msg = NULL;
    switch (serial_transport_get_interface()) {
    case SERIAL_IF_PORTA:
        ready_msg = "Port A UART ready (TX=GPIO53, RX=GPIO54).\r\n\r\n";
        break;
    case SERIAL_IF_MBUS:
        ready_msg = "MBUS UART2 ready (TX=GPIO6, RX=GPIO7; pins 16/15).\r\n\r\n";
        break;
    case SERIAL_IF_USB:
    default:
        ready_msg = "Connect a USB-serial device to the USB-A port.\r\n\r\n";
        break;
    }
    for (const char *p = ready_msg; *p; p++) vt100_process_byte((uint8_t)*p);
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
        const char *warn = "\033[1;33m[WARNING] Keyboard not detected!\033[0m\r\n";
        for (const char *p = warn; *p; p++) vt100_process_byte((uint8_t)*p);
        term_refresh_display();
    }

    update_status_bar();
    ESP_LOGI(TAG, "Entering main loop");

    // ---- Main loop ----
    RingbufHandle_t rx_rb       = usb_get_rx_ringbuf();
    QueueHandle_t   screen_logq = usb_get_screen_log_queue();
    QueueHandle_t   key_q       = usb_get_key_queue();

    while (1) {
        bool need_refresh = false;

        // 1. Process screen log messages from other tasks
        screen_log_msg_t log_msg;
        while (xQueueReceive(screen_logq, &log_msg, 0) == pdTRUE) {
            for (const char *p = log_msg.msg; *p; p++) vt100_process_byte((uint8_t)*p);
            need_refresh = true;
        }

        // 2. Process USB RX data through VT100 parser (drain ring buffer completely).
        //    While the settings screen is open, data is still parsed into term_buffer
        //    (keeping terminal state up to date) but term_refresh_display() is NOT
        //    called so the LVGL overlay remains visible undisturbed.
        //    When the settings screen closes, settings_ui_close() calls
        //    term_mark_all_dirty() + term_refresh_display() to show the updated screen.
        {
            size_t rx_len = 0;
            uint8_t *rx_data = (uint8_t *)xRingbufferReceiveUpTo(rx_rb, &rx_len, 0, 512);
            while (rx_data != NULL && rx_len > 0) {
                for (size_t i = 0; i < rx_len; i++) vt100_process_byte(rx_data[i]);
                vRingbufferReturnItem(rx_rb, rx_data);
                if (!settings_ui_is_open()) need_refresh = true;
                rx_data = (uint8_t *)xRingbufferReceiveUpTo(rx_rb, &rx_len, 0, 512);
            }
        }

        if (need_refresh) {
            term_refresh_display();
            update_status_bar();
        }

        // 3. Batch-save only in the configured deferred-learning mode. Manual
        // mode is saved by Dictionary Editor actions; Off never learns.
        service_deferred_learning();

        // 4. Process keyboard input (5 ms wait to keep USB RX responsive)
        key_event_msg_t key_msg;
        if (xQueueReceive(key_q, &key_msg, pdMS_TO_TICKS(5)) == pdTRUE) {
            bool kb_refresh = handle_key_event(&key_msg);
            if (kb_refresh) {
                term_refresh_display();
                update_status_bar();
            }
        }
    }
}

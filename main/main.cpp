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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

#include "m5_tab5_component.h"
#include "m5_tab5_keyboard.h"
#include "m5tab5_pinmap.h"

#include "terminal.h"
#include "display.h"
#include "usb_serial.h"
#include "settings.h"
#include "settings_ui.h"

static const char *TAG = "main";

static m5::tab5::m5tab5_component s_tab5_board;
static m5::M5Tab5Keyboard         s_keyboard;

// Current application settings (loaded from NVS at boot)
static app_settings_t s_settings = {};

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

    // ---- Ctrl+Alt combinations (local shortcuts, never sent to remote) ----
    if (ctrl && alt) {
        char k = (char)toupper((unsigned char)msg->str[0]);
        if (k == 'S') {
            // Ctrl+Alt+S: toggle settings screen
            if (settings_ui_is_open()) {
                settings_ui_close();
            } else {
                settings_ui_open(&s_settings);
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

    // ---- Ctrl-only combinations ----
    if (ctrl) {
        char k = (char)toupper((unsigned char)msg->str[0]);

        if (k == 'C') {
            // Ctrl+C: clear screen locally
            term_clear_all();
            const char *m = "\033[1;32m[Screen cleared]\033[0m\n";
            for (const char *p = m; *p; p++) vt100_process_byte((uint8_t)*p);
            return true;
        }
        if (k == 'L') {
            // Ctrl+L: force full redisplay
            term_mark_all_dirty();
            return true;
        }

        // Ctrl+ESC → send ESC
        if (strcasecmp(msg->str, "escape") == 0 || strcasecmp(msg->str, "esc") == 0) {
            uint8_t esc = 0x1B;
            usb_tx(&esc, 1);
            return false;
        }

        // Other Ctrl+key: send as control character (e.g. Ctrl+D → 0x04)
        if (k >= '@' && k <= '_') {
            uint8_t ctrl_char = (uint8_t)(k - '@');
            usb_tx(&ctrl_char, 1);
        }
        return false;
    }

    // ---- Alt-only combinations: send ESC + key (standard terminal convention) ----
    if (alt) {
        uint8_t esc = 0x1B;
        usb_tx(&esc, 1);
        // Fall through to send the key itself
    }

    // Special key names → VT100 sequences
    for (int i = 0; s_special_keys[i].name != NULL; i++) {
        if (strcasecmp(msg->str, s_special_keys[i].name) == 0) {
            const char *seq = s_special_keys[i].seq;
            usb_tx((const uint8_t *)seq, strlen(seq));
            return false;
        }
    }

    // Normal printable characters: send to USB (remote device echoes back)
    for (int i = 0; msg->str[i] != '\0'; i++) {
        uint8_t c = (uint8_t)msg->str[i];
        usb_tx(&c, 1);
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
        "Initializing USB host...\r\n";
    for (const char *p = welcome; *p; p++) vt100_process_byte((uint8_t)*p);
    term_refresh_display();

    // ---- USB init (queues, ring buffer, semaphores) ----
    usb_init();

    // ---- Apply remaining settings (baud rate, log level) ----
    // font_size is already applied above; settings_apply() calls
    // ui_rebuild_for_font_size() only if needed, but since we already
    // set the correct size, the rebuild is a no-op in terms of geometry.
    // To avoid a redundant UI rebuild at startup, apply only baud/log here.
    usb_set_baud_rate(s_settings.baud_rate);
    {
        static const esp_log_level_t lvl_map[] = {
            ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN,
            ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
        };
        int idx = (int)s_settings.log_level;
        if (idx < 0) idx = 0;
        if (idx > 5) idx = 5;
        esp_log_level_set("*", lvl_map[idx]);
    }

    // ---- Start VCP task (also starts usb_lib_task internally) ----
    usb_start_vcp_task();

    // Wait for USB host to be ready (vcp_task notifies us)
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "USB ready, starting main loop");

    const char *ready_msg = "Connect a USB-serial device to the USB-A port.\r\n\r\n";
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

        // 3. Process keyboard input (5 ms wait to keep USB RX responsive)
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

#pragma once
/*
 * settings.h — Persistent application settings (stored in NVS).
 *
 * Settings are loaded at boot and saved whenever the user closes
 * the settings screen.  All values are stored under NVS namespace
 * "term_cfg".
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdbool.h>

// ==============================================================
// Enumerations
// ==============================================================

/** Serial interface selection. */
typedef enum {
    SERIAL_IF_USB = 0,  /**< USB-A CDC-ACM / VCP serial device */
    SERIAL_IF_PORTA,    /**< Port A TTL UART: GPIO53 TX, GPIO54 RX */
    SERIAL_IF_MBUS,     /**< M-Bus TTL UART: GPIO6 TX, GPIO7 RX (UART2) */
} serial_if_t;

/** ESP-IDF log level selection. */
typedef enum {
    LOG_LEVEL_NONE    = 0,
    LOG_LEVEL_ERROR   = 1,
    LOG_LEVEL_WARN    = 2,
    LOG_LEVEL_INFO    = 3,
    LOG_LEVEL_DEBUG   = 4,
    LOG_LEVEL_VERBOSE = 5,
} app_log_level_t;

/**
 * Font size selection.
 *   Small: 16px font → 160 cols × 43 rows (more information, smaller text)
 *   Large: 28px font →  91 cols × 25 rows (easier to read)
 */
typedef enum {
    FONT_SIZE_SMALL = 0,  /**< 16px IPA Gothic: 160×43 */
    FONT_SIZE_LARGE = 1,  /**< 28px IPA Gothic:  91×25 */
} app_font_size_t;

/**
 * Local keyboard echo on the TAB5 display. This setting never sends a
 * configuration command to the connected serial device.
 */
typedef enum {
    LOCAL_ECHO_OFF = 0,  /**< Display only text received from the peer */
    LOCAL_ECHO_ON  = 1,  /**< Also render locally transmitted key input */
} local_echo_t;

/** Default local keyboard input mode. Japanese mode performs TAB5-side
 *  romaji/SKK conversion and sends only committed UTF-8 text to the peer. */
typedef enum {
    INPUT_MODE_DIRECT   = 0,  /**< Send keyboard input directly to the peer */
    INPUT_MODE_JAPANESE = 1,  /**< Use TAB5 local Japanese input at startup */
} app_input_mode_t;

/** Punctuation conversion used while TAB5 Japanese input is active. */
typedef enum {
    PUNCTUATION_JAPANESE = 0, /**< . , - → 。 、 ー */
    PUNCTUATION_ASCII    = 1, /**< . , - remain ASCII */
    PUNCTUATION_FULLWIDTH = 2, /**< . , - → ． ， － */
} app_punctuation_style_t;

/** How automatic SKK candidate learning is persisted. */
typedef enum {
    LEARNING_SAVE_OFF      = 0, /**< Do not learn candidate priority. */
    LEARNING_SAVE_DEFERRED = 1, /**< Learn in RAM and batch-save automatically. */
    LEARNING_SAVE_MANUAL   = 2, /**< Learn in RAM; save only on explicit user action. */
} app_learning_save_mode_t;

// ==============================================================
// Settings Structure
// ==============================================================

typedef struct {
    uint32_t        baud_rate;   /**< Serial baud rate (default: 115200)    */
    serial_if_t     serial_if;   /**< Interface: USB, PortA, or M-Bus UART  */
    app_log_level_t log_level;   /**< ESP-IDF log level                     */
    app_font_size_t font_size;   /**< Terminal font size (Small or Large)   */
    local_echo_t    local_echo;  /**< Local TAB5 keyboard echo setting       */
    app_input_mode_t input_mode; /**< Default local keyboard input mode       */
    app_punctuation_style_t punctuation_style; /**< Japanese input punctuation style */
    app_learning_save_mode_t learning_save_mode; /**< SKK candidate learning persistence */
} app_settings_t;

// ==============================================================
// Default Values
// ==============================================================

#define SETTINGS_DEFAULT_BAUD       115200
#define SETTINGS_DEFAULT_SERIAL_IF  SERIAL_IF_USB
#define SETTINGS_DEFAULT_LOG_LEVEL  LOG_LEVEL_INFO
#define SETTINGS_DEFAULT_FONT_SIZE  FONT_SIZE_LARGE
#define SETTINGS_DEFAULT_LOCAL_ECHO LOCAL_ECHO_OFF
#define SETTINGS_DEFAULT_INPUT_MODE INPUT_MODE_DIRECT
#define SETTINGS_DEFAULT_PUNCTUATION_STYLE PUNCTUATION_JAPANESE
#define SETTINGS_DEFAULT_LEARNING_SAVE_MODE LEARNING_SAVE_DEFERRED

// ==============================================================
// API
// ==============================================================

/**
 * @brief Load settings from NVS.  If NVS has no saved values,
 *        defaults are returned.  Must be called once at boot.
 */
void settings_load(app_settings_t *out);

/**
 * @brief Save settings to NVS.
 * @return true on success.
 */
bool settings_save(const app_settings_t *s);

/**
 * @brief Apply settings to the running system
 *        (baud rate, log level, font size, etc.).
 *
 * Note: font_size change triggers ui_rebuild_for_font_size() which
 * clears the screen and rebuilds LVGL objects.
 */
void settings_apply(const app_settings_t *s);

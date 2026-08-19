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

// ==============================================================
// Settings Structure
// ==============================================================

typedef struct {
    uint32_t        baud_rate;   /**< Serial baud rate (default: 115200)    */
    serial_if_t     serial_if;   /**< Interface: USB, PortA, or M-Bus UART  */
    app_log_level_t log_level;   /**< ESP-IDF log level                     */
    app_font_size_t font_size;   /**< Terminal font size (Small or Large)   */
} app_settings_t;

// ==============================================================
// Default Values
// ==============================================================

#define SETTINGS_DEFAULT_BAUD       115200
#define SETTINGS_DEFAULT_SERIAL_IF  SERIAL_IF_USB
#define SETTINGS_DEFAULT_LOG_LEVEL  LOG_LEVEL_INFO
#define SETTINGS_DEFAULT_FONT_SIZE  FONT_SIZE_LARGE

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

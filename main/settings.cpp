/*
 * settings.cpp — Persistent application settings (NVS backend).
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings.h"
#include "serial_transport.h"
#include "display.h"
#include "terminal.h"

#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char *TAG        = "settings";
static const char *NVS_NS     = "term_cfg";
static const char *KEY_BAUD   = "baud";
static const char *KEY_IFACE  = "iface";
static const char *KEY_LOG    = "log_level";
static const char *KEY_FONT   = "font_size";

// ==============================================================
// Load
// ==============================================================

void settings_load(app_settings_t *out)
{
    // Fill with defaults first
    out->baud_rate  = SETTINGS_DEFAULT_BAUD;
    out->serial_if  = SETTINGS_DEFAULT_SERIAL_IF;
    out->log_level  = SETTINGS_DEFAULT_LOG_LEVEL;
    out->font_size  = SETTINGS_DEFAULT_FONT_SIZE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved settings, using defaults");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    uint32_t v32 = 0;
    if (nvs_get_u32(h, KEY_BAUD, &v32) == ESP_OK) {
        out->baud_rate = v32;
    }
    uint8_t v8 = 0;
    if (nvs_get_u8(h, KEY_IFACE, &v8) == ESP_OK) {
        // Preserve old USB/Port A settings and accept the new MBUS value.
        // Invalid persisted values are safely reset to the USB default.
        out->serial_if = (v8 <= (uint8_t)SERIAL_IF_MBUS)
                         ? (serial_if_t)v8 : SETTINGS_DEFAULT_SERIAL_IF;
    }
    if (nvs_get_u8(h, KEY_LOG, &v8) == ESP_OK) {
        out->log_level = (app_log_level_t)v8;
    }
    if (nvs_get_u8(h, KEY_FONT, &v8) == ESP_OK) {
        out->font_size = (app_font_size_t)v8;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Settings loaded: baud=%"PRIu32" iface=%d log=%d font=%d",
             out->baud_rate, (int)out->serial_if, (int)out->log_level, (int)out->font_size);
}

// ==============================================================
// Save
// ==============================================================

bool settings_save(const app_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(RW) failed: %s", esp_err_to_name(err));
        return false;
    }

    bool ok = true;
    ok &= (nvs_set_u32(h, KEY_BAUD,  s->baud_rate)           == ESP_OK);
    ok &= (nvs_set_u8 (h, KEY_IFACE, (uint8_t)s->serial_if)  == ESP_OK);
    ok &= (nvs_set_u8 (h, KEY_LOG,   (uint8_t)s->log_level)  == ESP_OK);
    ok &= (nvs_set_u8 (h, KEY_FONT,  (uint8_t)s->font_size)  == ESP_OK);
    ok &= (nvs_commit(h) == ESP_OK);

    nvs_close(h);
    if (ok) {
        ESP_LOGI(TAG, "Settings saved: baud=%"PRIu32" iface=%d log=%d font=%d",
                 s->baud_rate, (int)s->serial_if, (int)s->log_level, (int)s->font_size);
    } else {
        ESP_LOGE(TAG, "Settings save failed (partial write)");
    }
    return ok;
}

// ==============================================================
// Apply
// ==============================================================

void settings_apply(const app_settings_t *s)
{
    // Select the requested transport and apply its baud rate. Switching
    // interfaces stops the previous transport before starting the new one.
    esp_err_t transport_err = serial_transport_select(s->serial_if, s->baud_rate);
    if (transport_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select %s transport: %s",
                 serial_transport_get_name(), esp_err_to_name(transport_err));
    }

    // Apply log level to ESP-IDF logging system
    static const esp_log_level_t lvl_map[] = {
        ESP_LOG_NONE,
        ESP_LOG_ERROR,
        ESP_LOG_WARN,
        ESP_LOG_INFO,
        ESP_LOG_DEBUG,
        ESP_LOG_VERBOSE,
    };
    int idx = (int)s->log_level;
    if (idx < 0) idx = 0;
    if (idx > 5) idx = 5;
    esp_log_level_set("*", lvl_map[idx]);
    ESP_LOGI(TAG, "Log level set to %d", idx);

    // Apply font size only when it differs from the active geometry.
    // A real size change clears the terminal and rebuilds the display.
    // Small: 16px font (8px half-width)  → 160 cols × 43 rows
    // Large: 28px font (14px half-width) →  91 cols × 25 rows
    const int wanted_font_w = (s->font_size == FONT_SIZE_SMALL) ? 8 : 14;
    const int wanted_font_h = (s->font_size == FONT_SIZE_SMALL) ? 16 : 28;
    if (TERM_FONT_W != wanted_font_w || TERM_FONT_H != wanted_font_h) {
        ui_rebuild_for_font_size(wanted_font_w, wanted_font_h);
    }

    // Notify a connected peer about the active terminal geometry. For USB
    // without a device this is harmless; the connection path sends it again.
    if (transport_err == ESP_OK) {
        serial_transport_send_window_size();
    }

    // Refresh the status bar after a baud-rate or interface change.
    update_status_bar();
}

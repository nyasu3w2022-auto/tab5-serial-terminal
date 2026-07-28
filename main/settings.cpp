/*
 * settings.cpp — Persistent application settings (NVS backend).
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings.h"
#include "usb_serial.h"

#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char *TAG       = "settings";
static const char *NVS_NS    = "term_cfg";
static const char *KEY_BAUD  = "baud";
static const char *KEY_IFACE = "iface";
static const char *KEY_LOG   = "log_level";

// ==============================================================
// Load
// ==============================================================

void settings_load(app_settings_t *out)
{
    // Fill with defaults first
    out->baud_rate  = SETTINGS_DEFAULT_BAUD;
    out->serial_if  = SETTINGS_DEFAULT_SERIAL_IF;
    out->log_level  = SETTINGS_DEFAULT_LOG_LEVEL;

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
        out->serial_if = (serial_if_t)v8;
    }
    if (nvs_get_u8(h, KEY_LOG, &v8) == ESP_OK) {
        out->log_level = (app_log_level_t)v8;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Settings loaded: baud=%"PRIu32" iface=%d log=%d",
             out->baud_rate, (int)out->serial_if, (int)out->log_level);
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
    ok &= (nvs_set_u32(h, KEY_BAUD,  s->baud_rate)          == ESP_OK);
    ok &= (nvs_set_u8 (h, KEY_IFACE, (uint8_t)s->serial_if) == ESP_OK);
    ok &= (nvs_set_u8 (h, KEY_LOG,   (uint8_t)s->log_level) == ESP_OK);
    ok &= (nvs_commit(h) == ESP_OK);

    nvs_close(h);
    if (ok) {
        ESP_LOGI(TAG, "Settings saved: baud=%"PRIu32" iface=%d log=%d",
                 s->baud_rate, (int)s->serial_if, (int)s->log_level);
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
    // Apply baud rate to USB serial module
    usb_set_baud_rate(s->baud_rate);

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

    // serial_if: PortA is not yet implemented; USB is always active.
    if (s->serial_if == SERIAL_IF_PORTA) {
        ESP_LOGW(TAG, "PortA interface not yet implemented, staying on USB");
    }
}

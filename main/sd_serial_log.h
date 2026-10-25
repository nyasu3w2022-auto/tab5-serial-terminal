#pragma once
/*
 * sd_serial_log.h — Asynchronous raw serial RX logging to Tab5 microSD.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** User-visible runtime condition of the serial RX logger. */
typedef enum {
    SD_SERIAL_LOG_OFF = 0,  /**< Logging disabled by the persistent setting. */
    SD_SERIAL_LOG_ON,       /**< Appending raw RX bytes to the SD file. */
    SD_SERIAL_LOG_NO_SD,    /**< SD card missing or FAT32 mount failed. */
    SD_SERIAL_LOG_LOW,      /**< Stopped before consuming dictionary safety reserve. */
    SD_SERIAL_LOG_FULL,     /**< Write returned ENOSPC or a partial byte count. */
    SD_SERIAL_LOG_RO,       /**< The medium is read-only or write access was denied. */
    SD_SERIAL_LOG_IO,       /**< SDMMC, file-write, or sync failure. */
    SD_SERIAL_LOG_MEM,      /**< PSRAM logger queue or worker could not be created. */
} sd_serial_log_status_t;

/** Fixed 8.3-compatible log path. The byte stream is raw RX data, with no header. */
constexpr const char *SD_SERIAL_LOG_DIRECTORY = "/sd/TAB5-LOG";
constexpr const char *SD_SERIAL_LOG_FILE_PATH = "/sd/TAB5-LOG/TERMLOG.TXT";

/**
 * Create the PSRAM RX-log queue and the low-priority SD writer task.
 * Logging remains disabled until sd_serial_log_configure(true) is called.
 */
bool sd_serial_log_init();

/** Enable or disable logging without blocking the serial receive path. */
void sd_serial_log_configure(bool enabled);

/** Return whether persistent configuration currently requests logging. */
bool sd_serial_log_is_enabled();

/**
 * Copy received bytes into the log queue without waiting. This is safe to call
 * only from the normal application task, immediately after taking a shared RX
 * ring-buffer item. Failure intentionally never affects terminal parsing.
 */
void sd_serial_log_enqueue_rx(const uint8_t *data, size_t len);

/** Return the compact status text intended for the terminal status line. */
const char *sd_serial_log_status_text();

/** Return the current logger status as an enum for diagnostics and tests. */
sd_serial_log_status_t sd_serial_log_status();

/**
 * Return and clear a status-change notification. The main task calls
 * update_status_bar() when true, keeping all LVGL calls out of the writer task.
 */
bool sd_serial_log_take_status_changed();

/**
 * Temporarily close and synchronize the logger file before an SD dictionary
 * transfer. RX continues accumulating in the PSRAM logger queue while paused.
 * Returns false only when a running logger did not acknowledge before timeout.
 */
bool sd_serial_log_pause_for_sd_operation(uint32_t timeout_ms);

/** Resume appending after the matching pause_for_sd_operation() call. */
void sd_serial_log_resume_after_sd_operation();

#pragma once
/*
 * sd_dictionary.h — Shared Tab5 microSD session and SKK dictionary transfer API.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "esp_err.h"
#include "dictionary_transfer.h"

/** microSD mount point and portable user dictionary exchange path. */
constexpr const char *SD_DICTIONARY_MOUNT_POINT = "/sd";
constexpr const char *SD_DICTIONARY_DIRECTORY = "/sd/TAB5-SKK";
// Use an 8.3-compatible name because FATFS long-file-name support is optional.
constexpr const char *SD_DICTIONARY_FILE_PATH = "/sd/TAB5-SKK/SKKUSER.TXT";

/** Initialize the recursive mutex used by every SDMMC/FATFS consumer. */
bool sd_dictionary_init();

/**
 * Acquire exclusive access to the Tab5 SDMMC/FATFS session.
 * `timeout_ms == 0` waits indefinitely. Call sd_dictionary_unlock() exactly
 * once after a successful lock.
 */
bool sd_dictionary_lock(uint32_t timeout_ms);
void sd_dictionary_unlock();

/** Mount Tab5's microSD card as a FAT filesystem, without formatting it. */
esp_err_t sd_dictionary_mount();

/** Same as sd_dictionary_mount(), but requires sd_dictionary_lock() ownership. */
esp_err_t sd_dictionary_mount_locked();

/** Unmount the microSD card if currently mounted. */
void sd_dictionary_unmount();

/** Same as sd_dictionary_unmount(), but requires sd_dictionary_lock() ownership. */
void sd_dictionary_unmount_locked();

/** True if the FAT filesystem is mounted at SD_DICTIONARY_MOUNT_POINT. */
bool sd_dictionary_is_mounted();

/**
 * Return FAT volume capacity and free bytes. Requires sd_dictionary_lock()
 * ownership and a mounted card. This uses ESP-IDF's FatFs VFS query API.
 */
esp_err_t sd_dictionary_get_space_info(uint64_t *total_bytes, uint64_t *free_bytes);

/** Export current SPIFFS user dictionary to the standard SD exchange file. */
dictionary_transfer_result_t sd_dictionary_export_user(const char *user_dictionary_path);

/** Import the standard SD exchange file into the SPIFFS user dictionary. */
dictionary_transfer_result_t sd_dictionary_import_user(const char *user_dictionary_path,
                                                       dictionary_transfer_mode_t mode);
